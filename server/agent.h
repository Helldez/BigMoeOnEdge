// The agent's run loop and scheduler (docs/agent.md).
//
// A message starts a run on a thread of the agent's: one run per conversation, and its rounds line
// up behind the engine's turn queue like any chat turn (EngineHost::chat). A round asks the model
// with the conversation as it stands, publishes what it writes as `delta` events, and runs the tool
// calls it made under the conversation's mode: a read runs, a change runs, waits for a person or is
// refused, and the results go back to the model in the next round, until an answer that calls no
// tool or the round limit. Everything a run produces is an item of the conversation, written to the
// store and published as it changes, so a UI that reconnects reads the whole state from the store.
//
// Threads. Each run is one thread, which also runs its tools; an approval is a condition variable
// that thread waits on, answered by approve() or stop() from an HTTP worker. The scheduler is one
// more thread. shutdown() raises every run's stop, which ends a waiting approval at once, a command
// by killing its process group, an MCP call by ending its server, a provider's stream at its next
// piece, and the engine's turn at its next token, then waits for all of them and ends the MCP
// servers: an embedding app's stop (server.h) returns only once nothing of the agent is running.
#pragma once

#include "agent_schedule.h"
#include "agent_skills.h"
#include "agent_store.h"
#include "chat_turn.h"
#include "engine_host.h"
#include "event_bus.h"
#include "http_fetch.h"
#include "json_rpc.h"
#include "json_util.h"
#include "mcp.h"
#include "platform.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace bmoe::server::agent {

// Rounds one message may take: model answer, tool calls, results, again. A task that needs more is
// stopped with a notice and can be continued with another message.
inline constexpr int k_max_rounds_per_message = 30;
// The same tool call failing the same way this many times in a row stops the run: a model that
// repeats a refused call is not going to get a different answer.
inline constexpr int k_max_same_failed_calls = 3;
// The most of a workspace's AGENTS.md the system prompt carries.
inline constexpr size_t k_agents_md_max_bytes = 8 * 1024;
// How long a stop waits for the run to end before answering anyway: the engine's turn ends at its
// next token, which a long prompt's prefill can delay.
inline constexpr int k_stop_wait_s = 30;
// How often the scheduler looks at the tasks when nothing wakes it.
inline constexpr int k_scheduler_poll_s = 20;
// How long a provider's answer may take, whole.
inline constexpr int k_provider_timeout_s = 600;
inline constexpr size_t k_title_max_chars = 60;

// The model a round asks: the engine's turn, or a test's stand-in.
using ChatFn = std::function<ChatOutcome(const ChatRequest & req,
                                         const std::function<bool(const TokenMetrics &)> & on_token,
                                         const std::function<bool()> & still_wanted)>;

struct AgentDeps {
    EventBus * bus = nullptr;
    AgentStore * store = nullptr;
    ChatFn local_chat;
    // n_predict and think for a round, from the server's settings; null keeps the server defaults.
    std::function<RunConfig()> config;
    HttpFetch * net = nullptr;           // the providers; null: none
    McpHub * mcp = nullptr;              // the MCP servers' tools; null: none
    std::string bundled_skills_dir;      // `skills/` beside the executable
    std::string user_skills_dir;         // `<data>/skills`
    bool tools = true;                   // false: plain chat (--chat-only), no tools and no agent prompt
    bool can_run_commands = false;       // platform::can_run_commands()
    std::string os_name = "this system"; // for the system prompt
    // Runs a command (platform::run_command); a test passes its own.
    std::function<platform::CommandResult(const std::string & command,
                                          const std::string & cwd,
                                          int timeout_s,
                                          size_t max_output,
                                          const std::atomic<bool> & stop)>
        run_command;
    // The other coding agents a conversation may use instead of the built-in one, over the Agent
    // Client Protocol: the catalog's entries that carry `acp` ({id, name, acp: {command, needs}}).
    json external_agents = json::array();
    // Starts one (`command` in `cwd`); a test passes an in-process agent. Null: platform::LineProcess.
    std::function<std::unique_ptr<JsonRpcTransport>(
        const std::vector<std::string> & command, const std::string & cwd, std::string & err)>
        start_agent;
    // Whether a program is installed (platform::find_program); a test passes its own.
    std::function<bool(const std::string & program)> program_found;
};

// The agent a conversation uses when it names none: this server's own.
inline constexpr const char * k_builtin_agent = "builtin";
// How long an agent asked to stop has to answer before it is ended.
inline constexpr int k_external_cancel_grace_s = 10;

// Who may see a conversation's events: local clients only when it reaches this computer
// (conversation_reaches).
EventBus::Audience conversation_audience(const json & conv);

// What a request that is refused gets: status 0 means accepted.
struct Refusal {
    int status = 0;
    std::string error;
    std::string code;
    bool ok() const { return status == 0; }
};

class Agent {
public:
    explicit Agent(AgentDeps deps);
    ~Agent();
    Agent(const Agent &) = delete;
    Agent & operator=(const Agent &) = delete;

    // Start the task scheduler. A test drives tasks with run_task() instead.
    void start_scheduler();
    // Stop every run and the scheduler, and wait for all of them. Idempotent.
    void shutdown();

    // A message: add it and start the run. `remote`: the request came from beyond loopback; the
    // run then offers no file, command or MCP tool. `item_out` is the user item.
    Refusal send(const std::string & conversation, const std::string & text, bool think, bool remote, json & item_out);
    // Drop `item` (a user item) and everything after it, then send `text`.
    Refusal edit(const std::string & conversation,
                 const std::string & item,
                 const std::string & text,
                 bool think,
                 bool remote,
                 json & item_out);
    Refusal approve(const std::string & conversation, const std::string & item, bool approved);
    // Stop the run of `conversation` and wait for it to end. False when none runs.
    bool stop(const std::string & conversation);
    bool running(const std::string & conversation) const;

    // The conversation as the API shows it: the stored one, with `state` from the live run.
    bool conversation(const std::string & id, json & out) const;
    json list_conversations() const;
    // What a conversation here can do: {tools, commands, agents, mcp}, for the UIs to show only what
    // works. `agents` lists the built-in agent and each external one with whether it is installed
    // (and, when not, which program is missing); it is empty where commands cannot run. `mcp` lists
    // the MCP servers with their tools (McpHub::describe), starting them, when `with_mcp`; else [].
    json capabilities(bool with_mcp) const;
    // Whether `id` names an agent a conversation may use here (installed or not).
    bool knows_agent(const std::string & id) const;
    // Remove one that is not running.
    Refusal remove_conversation(const std::string & id);

    // Every skill the agent sees with `workspace_path` ("" for none): bundled, then the person's own,
    // then the workspace's, a later one replacing an earlier one of the same name; with `enabled`.
    std::vector<std::pair<Skill, bool>> skills(const std::string & workspace_path,
                                               std::vector<std::string> * warnings = nullptr) const;

    // Run a task now, as the scheduler would. False when it is already running or does not exist.
    Refusal run_task(const std::string & id);
    // A task was added, changed or removed: the scheduler looks again.
    void tasks_changed();
    // The task with `next_run` filled in.
    json task_view(const json & task) const;

private:
    struct Run;
    struct External;

    Refusal start(const std::string & conversation,
                  const std::string & text,
                  bool think,
                  bool remote,
                  bool auto_deny,
                  const std::string & task_id,
                  json & item_out);
    // One conversation's rounds, on the run's thread; the final answer's text.
    std::string run_rounds(Run & run, const std::string & conv_id);
    json run_tool(Run & run, const json & conv, const ChatToolCall & call);
    bool wait_approval(Run & run, const std::string & conv_id, json & item);
    ChatOutcome ask_model(Run & run,
                          const json & conv,
                          const ChatRequest & req,
                          const std::function<bool(const TokenMetrics &)> & on_token);
    ChatOutcome ask_provider(Run & run,
                             const std::string & route,
                             const ChatRequest & req,
                             const std::function<bool(const TokenMetrics &)> & on_token);
    ChatRequest build_request(const json & conv, const std::string & system, bool remote, bool think) const;
    // The tools a round offers; `mcp` gets the MCP tools among them, by the name the model calls.
    std::vector<ChatTool>
    tools_for(const json & conv, bool remote, std::map<std::string, McpTool> * mcp = nullptr) const;
    std::string system_prompt(const json & conv, bool remote) const;

    // ---- a conversation whose agent is another program (agent_external.cpp) ----
    std::string run_external(Run & run, const std::string & conv_id);
    std::shared_ptr<External> external_for(Run & run, const json & conv, const std::string & ws, std::string & err);
    void on_external_update(const std::shared_ptr<External> & ext, const json & params);
    bool on_external_request(const std::shared_ptr<External> & ext,
                             const std::string & method,
                             const json & params,
                             json & result,
                             std::string & error);
    void close_external(const std::string & conv_id);

    // Who may see a conversation's events: local clients only when it reaches this computer.
    EventBus::Audience audience_of(const std::string & conv) const;
    void publish_conversation(const std::string & id);
    void publish_item(const std::string & conv, const json & item, bool persist = true);
    void notice(const std::string & conv, const std::string & level, const std::string & text);
    std::string live_state(const std::string & id, const std::string & stored) const;
    std::string workspace_path(const json & conv) const;
    void finish_task(const std::string & task_id, const std::string & conv_id, const std::string & outcome);
    void scheduler_loop();

    AgentDeps d_;

    mutable std::mutex m_; // runs_, threads, shutdown
    std::condition_variable cv_;
    std::map<std::string, std::shared_ptr<Run>> runs_; // every conversation a run is working on
    int active_ = 0;                                   // run threads alive
    // Runs whose thread has returned or is returning, joined at the next start or at shutdown.
    std::vector<std::pair<std::string, std::shared_ptr<Run>>> finished_;
    std::set<std::string> running_tasks_; // task ids with a run going
    // The external agents running, one per conversation, kept between its messages.
    std::map<std::string, std::shared_ptr<External>> externals_;
    bool shutting_down_ = false;

    std::thread scheduler_;
    std::mutex sched_m_;
    std::condition_variable sched_cv_;
    bool sched_wake_ = false;
};

} // namespace bmoe::server::agent
