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
// Threads. Each run is one thread, which also runs the run's helper conversations (delegate) and
// its tools; an approval is a condition variable that thread waits on, answered by approve() or
// stop() from an HTTP worker. The scheduler is one more thread. shutdown() raises every run's stop,
// which ends a waiting approval at once, a command by killing its process group, a web request or
// a provider's stream at its next piece, and the engine's turn at its next token, then waits for all
// of them: an embedding app's stop (server.h) returns only once nothing of the agent is running.
#pragma once

#include "agent_schedule.h"
#include "agent_skills.h"
#include "agent_store.h"
#include "chat_turn.h"
#include "engine_host.h"
#include "event_bus.h"
#include "http_fetch.h"
#include "json_util.h"
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
// A helper may not start helpers of its own, and one message may start this many.
inline constexpr int k_max_delegate_depth = 1;
inline constexpr int k_max_helpers_per_message = 4;
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
    HttpFetch * net = nullptr;           // the web tools and providers; null: no web, no providers
    std::string bundled_skills_dir;      // `skills/` beside the executable
    std::string user_skills_dir;         // `<data>/skills`
    bool can_run_commands = false;       // platform::can_run_commands()
    std::string os_name = "this system"; // for the system prompt
    // Runs a command (platform::run_command); a test passes its own.
    std::function<platform::CommandResult(const std::string & command,
                                          const std::string & cwd,
                                          int timeout_s,
                                          size_t max_output,
                                          const std::atomic<bool> & stop)>
        run_command;
};

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
    // run then offers no file, command or web tool. `item_out` is the user item.
    Refusal send(const std::string & conversation, const std::string & text, bool think, bool remote, json & item_out);
    // Drop `item` (a user item) and everything after it, then send `text`.
    Refusal edit(const std::string & conversation,
                 const std::string & item,
                 const std::string & text,
                 bool think,
                 bool remote,
                 json & item_out);
    Refusal approve(const std::string & conversation, const std::string & item, bool approved);
    // Stop the run of `conversation` (and its helpers) and wait for it to end. False when none runs.
    bool stop(const std::string & conversation);
    bool running(const std::string & conversation) const;

    // The conversation as the API shows it: the stored one, with `state` from the live run.
    bool conversation(const std::string & id, json & out) const;
    json list_conversations() const;
    // Remove one that is not running; its helpers go with it.
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

    Refusal start(const std::string & conversation,
                  const std::string & text,
                  bool think,
                  bool remote,
                  bool auto_deny,
                  const std::string & task_id,
                  json & item_out);
    // One conversation's rounds, on the run's thread; the final answer's text.
    std::string run_rounds(Run & run, const std::string & conv_id, int depth);
    json run_tool(Run & run, const json & conv, const ChatToolCall & call, int depth);
    bool wait_approval(Run & run, const std::string & conv_id, json & item);
    ChatOutcome ask_model(Run & run,
                          const json & conv,
                          const ChatRequest & req,
                          const std::function<bool(const TokenMetrics &)> & on_token);
    ChatOutcome ask_provider(Run & run,
                             const std::string & route,
                             const ChatRequest & req,
                             const std::function<bool(const TokenMetrics &)> & on_token);
    ChatRequest build_request(const json & conv, const std::string & system, bool remote, int depth, bool think) const;
    std::vector<ChatTool> tools_for(const json & conv, bool remote, int depth) const;
    std::string system_prompt(const json & conv, bool remote) const;

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
    bool shutting_down_ = false;

    std::thread scheduler_;
    std::mutex sched_m_;
    std::condition_variable sched_cv_;
    bool sched_wake_ = false;
};

} // namespace bmoe::server::agent
