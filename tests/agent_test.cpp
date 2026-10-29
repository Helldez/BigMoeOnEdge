// The agent (server/agent.h, docs/agent.md), driven without a model: the run loop over the engine
// host with a scripted session behind it, and the rules its tools and stores apply.
//
//   * the loop: a tool call waits for an approval, runs once approved and its result reaches the
//     model; a denial reaches the model as a refusal and changes nothing; read-only mode refuses a
//     change and offers no editing tool, while a command that only reads still runs; a stop while
//     waiting ends the run and denies the card; the round limit ends a run that never answers; a
//     context that runs short shortens the oldest tool result and says so; no model is a notice; a
//     task's approvals are refused at once; shutdown ends a run that waits for a person; the system
//     prompt stays short and carries the workspace's AGENTS.md;
//   * path confinement: `..`, absolute paths, links out of the workspace and dangling links;
//   * the read-only command classifier; the diff an edit shows; the workspace tools (read_file,
//     file_glob_search, grep_search, edit_file's edits, globs); skills' front matter; the
//     schedule's next run; a provider's streamed answer;
//   * MCP servers, in process: the handshake, tools named `<server>_<tool>`, a call's result, an
//     error, a time limit; through the agent, the modes' approvals and the rules on what offers them;
//   * connecting a client: a TOML block replaced, a JSON file merged, a backup kept;
//   * a real command: output, exit code, folder, time limit, stop, a background child, the cap;
//   * stores: a key is never in a listing or the providers file, and its file is the owner's only;
//   * which routes answer only to this computer.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "access.h"
#include "agent.h"
#include "json_rpc.h"
#include "mcp.h"
#include "agent_schedule.h"
#include "agent_skills.h"
#include "agent_store.h"
#include "agent_tools.h"
#include "connect.h"
#include "engine_host.h"
#include "provider_protocol.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace bmoe;
using namespace bmoe::server;
using namespace bmoe::server::agent;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++failures;
}

static std::string slurp(const fs::path & p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void spit(const fs::path & p, const std::string & s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

// A folder of its own per test, removed at the end.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string & name) {
        path = fs::temp_directory_path() / ("bmoe-agent-test-" + name + "-" + new_id(""));
        fs::create_directories(path);
        path = fs::canonical(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// ---------------------------------------------------------------------------------------------
// The scripted model

// One answer of the model: text, or tool calls, or a refusal for the context.
struct Turn {
    std::string text;
    std::vector<ChatToolCall> calls;
    bool context_full = false;
};

struct Script {
    std::mutex m;
    std::deque<Turn> turns;
    Turn fallback{"(the script ran out)", {}, false};
    std::vector<GenerateRequest> requests;
    int n_ctx = 32768;
};

class ScriptedSession final : public HostedSession {
public:
    explicit ScriptedSession(Script & s) : s_(s) {}
    RunResult generate(const GenerateRequest & req,
                       const std::function<void(const TokenMetrics &)> & on_token) override {
        Turn t;
        {
            std::lock_guard<std::mutex> lk(s_.m);
            s_.requests.push_back(req);
            if (!s_.turns.empty()) {
                t = s_.turns.front();
                s_.turns.pop_front();
            } else {
                t = s_.fallback;
            }
        }
        RunResult r;
        if (t.context_full) {
            r.error = "the conversation exceeds the session n_ctx";
            r.error_kind = RunError::ContextFull;
            return r;
        }
        // The answer arrives as two tokens, as a stream would show it.
        TokenMetrics m;
        m.text = t.text.substr(0, t.text.size() / 2);
        if (on_token) on_token(m);
        m.text = t.text;
        if (on_token) on_token(m);
        r.ok = true;
        r.cancelled = cancelled_.exchange(false);
        r.generated_text = t.text;
        r.tool_calls = t.calls;
        r.summary.n_generated = 2;
        return r;
    }
    DecideResult decide(const DecideRequest &) override { return {}; }
    void cancel() override { cancelled_ = true; }
    const std::string & arch() const override { return arch_; }
    int n_ctx() const override { return s_.n_ctx; }
    int n_expert_used() const override { return 8; }
    ThinkControl think_control() const override { return ThinkControl::Template; }
    double load_seconds() const override { return 0.0; }

private:
    Script & s_;
    std::atomic<bool> cancelled_{false};
    std::string arch_ = "fakemoe";
};

static ChatToolCall call(const std::string & name, const json & args) {
    return {"", name, args.dump()};
}

// Everything one loop test needs, on the heap (see engine_host_test.cpp on why: ThreadSanitizer and
// mutexes reused at one stack address).
struct Rig {
    TempDir data{"data"};
    TempDir ws{"ws"};
    Script script;
    std::unique_ptr<EventBus> bus = std::make_unique<EventBus>();
    std::unique_ptr<EngineHost> host;
    std::unique_ptr<AgentStore> store;
    std::unique_ptr<Agent> agent;
    std::atomic<int> commands{0};
    std::string workspace_id;

    explicit Rig(bool load = true, bool tools = true, const std::function<void(AgentDeps &)> & tweak = nullptr) {
        host = std::make_unique<EngineHost>(*bus, [this](const SessionConfig &, std::string &) {
            return std::unique_ptr<HostedSession>(new ScriptedSession(script));
        });
        if (load) {
            RunConfig c;
            c.model_path = "models/fake.gguf";
            host->load(c, c);
            for (int i = 0; i < 500 && !host->ready(); ++i)
                std::this_thread::sleep_for(2ms);
        }
        store = std::make_unique<AgentStore>(data.path.u8string());
        store->load();
        AgentDeps d;
        d.bus = bus.get();
        d.store = store.get();
        d.local_chat = [this](const ChatRequest & r, const std::function<bool(const TokenMetrics &)> & t,
                              const std::function<bool()> & w) { return host->chat(r, t, w); };
        d.bundled_skills_dir = (data.path / "bundled").u8string();
        d.user_skills_dir = (data.path / "skills").u8string();
        d.tools = tools;
        d.can_run_commands = true;
        d.run_command = [this](const std::string & cmd, const std::string &, int, size_t, const std::atomic<bool> &) {
            ++commands;
            platform::CommandResult r;
            r.started = true;
            r.exit_code = 0;
            r.output = "ran: " + cmd;
            return r;
        };
        if (tweak) tweak(d);
        agent = std::make_unique<Agent>(std::move(d));
        json w;
        std::string err;
        store->add_workspace(ws.path.u8string(), "ws", w, err);
        workspace_id = w.value("id", "");
    }
    ~Rig() {
        agent->shutdown();
        agent.reset();
        host.reset();
    }

    void say(std::vector<Turn> turns) {
        std::lock_guard<std::mutex> lk(script.m);
        for (Turn & t : turns)
            script.turns.push_back(std::move(t));
    }
    std::string conversation(const std::string & mode, bool workspace = true) {
        json f{{"mode", mode}};
        if (workspace) f["workspace"] = workspace_id;
        return store->create_conversation(f).value("id", "");
    }
    json conv(const std::string & id) {
        json c;
        agent->conversation(id, c);
        return c;
    }
    bool wait_state(const std::string & id, const std::string & want, std::chrono::milliseconds limit = 5s) {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end) {
            if (conv(id).value("state", "") == want) return true;
            std::this_thread::sleep_for(2ms);
        }
        return false;
    }
    // Wait for the run to end (the conversation is no longer running).
    bool wait_done(const std::string & id, std::chrono::milliseconds limit = 5s) {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end) {
            if (!agent->running(id)) return true;
            std::this_thread::sleep_for(2ms);
        }
        return false;
    }
    json items_of(const std::string & id, const std::string & kind) {
        json out = json::array();
        const json c = conv(id); // held: a loop over a member of a temporary reads freed memory
        for (const json & it : c["items"])
            if (it.value("kind", "") == kind) out.push_back(it);
        return out;
    }
    std::string waiting_item(const std::string & id) {
        const json c = conv(id);
        for (const json & it : c["items"])
            if (it.value("state", "") == "waiting") return it.value("id", "");
        return "";
    }
    GenerateRequest last_request() {
        std::lock_guard<std::mutex> lk(script.m);
        return script.requests.empty() ? GenerateRequest{} : script.requests.back();
    }
    size_t requests() {
        std::lock_guard<std::mutex> lk(script.m);
        return script.requests.size();
    }
};

static bool declares(const GenerateRequest & r, const std::string & tool) {
    for (const ChatTool & t : r.tools)
        if (t.name == tool) return true;
    return false;
}

static std::string last_tool_message(const GenerateRequest & r) {
    for (auto it = r.messages.rbegin(); it != r.messages.rend(); ++it)
        if (it->role == "tool") return it->content;
    return "";
}

static void loop_tests() {
    {
        auto rig = std::make_unique<Rig>();
        const std::string c = rig->conversation("ask");
        rig->say({{"", {call("write_file", {{"path", "notes/hello.txt"}, {"content", "hi\n"}})}}, {"Written.", {}}});
        json item;
        check(rig->agent->send(c, "write a file", false, false, item).ok(), "a message starts a run");
        check(item.value("kind", "") == "user" && item.value("text", "") == "write a file",
              "it answers with the user item");
        check(rig->wait_state(c, "waiting"), "ask mode: a write waits for an approval");
        const std::string w = rig->waiting_item(c);
        const json tool = rig->items_of(c, "tool")[0];
        check(!fs::exists(rig->ws.path / "notes" / "hello.txt"), "nothing has run when the card appears");
        check(tool.value("diff", "").find("+hi") != std::string::npos && tool.value("file", "") == "notes/hello.txt",
              "the card carries the diff and the file it will write");
        json again;
        check(rig->agent->send(c, "again", false, false, again).status == 409,
              "a second message while it runs is a 409");
        check(rig->agent->approve(c, "nope", true).status == 404,
              "an approval of an item that is not waiting is a 404");
        check(rig->agent->approve(c, w, true).ok(), "the approval is taken");
        check(rig->wait_done(c), "the run ends after the approval");
        check(slurp(rig->ws.path / "notes" / "hello.txt") == "hi\n", "the approved write happened");
        const json done = rig->items_of(c, "tool")[0];
        check(done.value("state", "") == "done", "the tool item reads done");
        check(last_tool_message(rig->last_request()).find("File written successfully.") == 0,
              "the model reads the result in the next round");
        const json answers = rig->items_of(c, "assistant");
        check(answers.size() == 2 && answers[1].value("text", "") == "Written." && !answers[1].value("streaming", true),
              "the final answer is the last assistant item, no longer streaming");
        check(answers[0]["tool_calls"].size() == 1 && !answers[0]["tool_calls"][0].value("id", "").empty(),
              "the round that called keeps its call, with an id");
        check(rig->conv(c).value("state", "") == "idle", "the conversation is idle again");
        check(rig->conv(c).value("title", "") == "write a file", "an untitled conversation takes its first message");
        const GenerateRequest r = rig->last_request();
        check(!r.messages.empty() && r.messages[0].role == "system" &&
                  r.messages[0].content.find("working directory") != std::string::npos &&
                  r.messages[0].content.find(rig->ws.path.u8string()) == std::string::npos &&
                  r.messages[0].content.find("Mode: ask") != std::string::npos,
              "the system prompt says the work is in the working directory, without its path, and the mode");
        GenerateRequest first;
        {
            std::lock_guard<std::mutex> lk(rig->script.m);
            first = rig->script.requests.front();
        }
        check(first.messages[0].content == r.messages[0].content && first.messages.size() < r.messages.size(),
              "rounds of one message share the system prompt and only append");
    }
    {
        auto rig = std::make_unique<Rig>();
        const std::string c = rig->conversation("ask");
        rig->say(
            {{"", {call("edit_file", {{"path", "a.txt"}, {"edits", {{{"old_text", "one"}, {"new_text", "two"}}}}})}},
             {"Fine, I did not change it.", {}}});
        spit(rig->ws.path / "a.txt", "one\n");
        json item;
        rig->agent->send(c, "edit", false, false, item);
        check(rig->wait_state(c, "waiting"), "an edit waits too");
        check(rig->agent->approve(c, rig->waiting_item(c), false).ok(), "a denial is taken");
        check(rig->wait_done(c), "the run ends after a denial");
        check(slurp(rig->ws.path / "a.txt") == "one\n", "a denied edit changes nothing");
        check(rig->items_of(c, "tool")[0].value("state", "") == "denied", "the tool item reads denied");
        check(last_tool_message(rig->last_request()).find("declined") != std::string::npos,
              "the model is told the person declined");
    }
    {
        auto rig = std::make_unique<Rig>();
        const std::string c = rig->conversation("read_only");
        rig->say({{"", {call("exec_shell_command", {{"command", "rm -rf build"}})}},
                  {"", {call("exec_shell_command", {{"command", "ls -la"}})}},
                  {"I can only read.", {}}});
        json item;
        rig->agent->send(c, "clean up", false, false, item);
        check(rig->wait_done(c), "a read-only run ends without asking anything");
        const json tools = rig->items_of(c, "tool");
        check(tools.size() == 2 && tools[0].value("state", "") == "denied" &&
                  tools[0].value("result", "").find("read-only") != std::string::npos,
              "read_only: a command that may change something is refused, and the model reads why");
        check(tools[1].value("state", "") == "done" && rig->commands == 1,
              "read_only: a command that only reads runs, and only that one ran");
        const GenerateRequest r = rig->last_request();
        check(!declares(r, "write_file") && !declares(r, "edit_file") && declares(r, "read_file") &&
                  declares(r, "exec_shell_command"),
              "read_only: the editing tools are not offered at all");
    }
    {
        auto rig = std::make_unique<Rig>();
        const std::string c = rig->conversation("ask");
        rig->say({{"", {call("exec_shell_command", {{"command", "make install"}})}}});
        json item;
        rig->agent->send(c, "install", false, false, item);
        check(rig->wait_state(c, "waiting"), "a command that may change something waits");
        const auto t0 = std::chrono::steady_clock::now();
        check(rig->agent->stop(c), "stop finds the run");
        check(std::chrono::steady_clock::now() - t0 < 3s && !rig->agent->running(c),
              "stop answers once the run has ended, without waiting for a person");
        check(rig->items_of(c, "tool")[0].value("state", "") == "denied" && rig->commands == 0,
              "a card a stop ended is denied, and nothing ran");
        const json notes = rig->items_of(c, "notice");
        check(!notes.empty() && notes[notes.size() - 1].value("text", "") == "Stopped.",
              "the stop is said in a notice");
        check(!rig->agent->stop(c), "stop with nothing running is false");
    }
    {
        auto rig = std::make_unique<Rig>();
        rig->script.fallback = {"", {call("file_glob_search", {{"path", "."}})}, false};
        const std::string c = rig->conversation("full");
        json item;
        rig->agent->send(c, "loop forever", false, false, item);
        check(rig->wait_done(c, 20s), "a model that never stops calling is stopped");
        check(rig->requests() == (size_t) k_max_rounds_per_message, "after exactly the round limit");
        const json notes = rig->items_of(c, "notice");
        check(!notes.empty() && notes[0].value("level", "") == "warn", "with a warning that says so");
    }
    {
        auto rig = std::make_unique<Rig>();
        spit(rig->ws.path / "big.txt", std::string(4000, 'x') + "\n");
        const std::string c = rig->conversation("full");
        rig->say({{"", {call("read_file", {{"path", "big.txt"}})}}, {"", {}, true}, {"Read it.", {}}});
        json item;
        rig->agent->send(c, "read", false, false, item);
        check(rig->wait_done(c), "a context refusal is not the end of the run");
        const json tool = rig->items_of(c, "tool")[0];
        check(tool.value("shortened", false) && tool.value("result", "").size() > 4000,
              "the oldest long tool result is marked shortened and kept whole for the person");
        check(last_tool_message(rig->last_request()).size() < 1000, "the model gets the shortened result");
        check(rig->items_of(c, "assistant").back().value("text", "") == "Read it.",
              "and the run goes on to its answer");
        check(rig->items_of(c, "notice").size() == 1, "a notice says the results were shortened");
    }
    {
        auto rig = std::make_unique<Rig>(false);
        const std::string c = rig->conversation("ask", false);
        json item;
        check(rig->agent->send(c, "hello", false, false, item).ok(), "a message with no model is accepted");
        check(rig->wait_done(c), "and its run ends");
        const json notes = rig->items_of(c, "notice");
        check(notes.size() == 1 && notes[0].value("level", "") == "error" &&
                  notes[0].value("text", "").find("No model") != std::string::npos,
              "no model loaded: an error notice says so");
        check(rig->conv(c).value("state", "") == "error", "the conversation reads error");
        check(rig->items_of(c, "user").size() == 1, "the person's message is kept");
    }
    {
        auto rig = std::make_unique<Rig>();
        json task{{"id", "t1"},
                  {"title", "nightly"},
                  {"prompt", "write the report"},
                  {"workspace", rig->workspace_id},
                  {"mode", "ask"},
                  {"route", "local"},
                  {"schedule", {{"kind", "daily"}, {"at", "03:00"}}},
                  {"enabled", true},
                  {"created", now_s()}};
        rig->store->put_task(task);
        rig->say({{"", {call("write_file", {{"path", "report.md"}, {"content", "x"}})}}, {"Could not write.", {}}});
        check(rig->agent->run_task("t1").ok(), "a task runs on request");
        json t;
        for (int i = 0; i < 2500; ++i) {
            rig->store->task("t1", t);
            if (t["last_run"].value("outcome", "") != "running") break;
            std::this_thread::sleep_for(2ms);
        }
        check(t["last_run"].value("outcome", "") == "done", "the task's run ends done");
        const std::string cid = t["last_run"].value("conversation", "");
        check(!fs::exists(rig->ws.path / "report.md"), "nobody at the screen: the write was not approved");
        const json tools = rig->items_of(cid, "tool");
        check(tools.size() == 1 && tools[0].value("state", "") == "denied", "the approval was answered no at once");
        bool reported = false;
        for (const json & n : rig->items_of(cid, "notice"))
            reported = reported || n.value("text", "").find("approval") != std::string::npos;
        check(reported, "the run says what was not done");
        check(rig->conv(cid).value("task", "") == "t1", "the run's conversation names its task");
    }
    {
        auto rig = std::make_unique<Rig>();
        const std::string c = rig->conversation("ask", false);
        rig->say({{"", {call("fetch_url", {{"url", "https://example.com"}})}}});
        json item;
        rig->agent->send(c, "fetch", false, false, item);
        // A tool this server does not have (the web is an MCP server's, when one is configured) is an
        // error, not a card.
        check(rig->wait_done(c), "a tool the conversation is not offered ends as an error item");
        check(rig->items_of(c, "tool")[0].value("state", "") == "error", "with state error");
    }
    {
        // --chat-only: the model gets the conversation alone, as plain chat did before the agent.
        auto rig = std::make_unique<Rig>(true, false);
        check(!rig->agent->capabilities(true).value("tools", true) &&
                  !rig->agent->capabilities(true).value("commands", true),
              "chat only: the server says it has no agent");
        const std::string c = rig->conversation("full");
        rig->say({{"Hello.", {}}});
        json item;
        rig->agent->send(c, "hi", false, false, item);
        check(rig->wait_done(c), "chat only: a message is answered");
        const GenerateRequest r = rig->last_request();
        check(r.tools.empty() && !r.messages.empty() && r.messages[0].role == "user",
              "chat only: no tools and no agent prompt, even with a workspace and full access");
    }
    {
        // A model that keeps asking for the same refused call is stopped, not left to loop.
        auto rig = std::make_unique<Rig>();
        const std::string c = rig->conversation("ask", false);
        std::vector<Turn> same;
        for (int i = 0; i < k_max_same_failed_calls + 3; ++i)
            same.push_back({"", {call("read_file", {{"path", "note.md"}})}});
        rig->say(same);
        json item;
        rig->agent->send(c, "read my note", false, false, item);
        check(rig->wait_done(c), "a repeated failing call ends the run");
        check(rig->items_of(c, "tool").size() == static_cast<size_t>(k_max_same_failed_calls),
              "it stops at the repeat limit");
        const json notices = rig->items_of(c, "notice");
        check(!notices.empty() && notices.back().value("text", "").find("same thing") != std::string::npos,
              "and says why");
        check(rig->last_request().messages[0].content.find("pick a folder") != std::string::npos,
              "without a workspace the prompt says to pick a folder for files");
    }
    {
        auto rig = std::make_unique<Rig>();
        const std::string c = rig->conversation("ask");
        rig->say({{"", {call("write_file", {{"path", "x.txt"}, {"content", "x"}})}}});
        json item;
        rig->agent->send(c, "write", false, false, item);
        check(rig->wait_state(c, "waiting"), "a run waits for a person");
        const auto t0 = std::chrono::steady_clock::now();
        rig->agent->shutdown();
        check(std::chrono::steady_clock::now() - t0 < 3s && !rig->agent->running(c),
              "shutdown ends a waiting run and returns (an embedding app's stop)");
        json late;
        check(rig->agent->send(c, "more", false, false, late).status == 503, "after shutdown no run starts");
    }
}

// ---------------------------------------------------------------------------------------------
// Rules

static void confinement_tests() {
    TempDir root("confine"), outside("outside");
    spit(root.path / "src" / "main.cpp", "int main() {}\n");
    spit(outside.path / "secret.txt", "secret\n");
    std::string out, err;
    const std::string r = root.path.u8string();
    check(resolve_in_workspace(r, "src/main.cpp", out, err) && out == (root.path / "src" / "main.cpp").u8string(),
          "a relative path resolves inside");
    check(resolve_in_workspace(r, "new/dir/file.txt", out, err), "a file that does not exist yet resolves inside");
    check(resolve_in_workspace(r, "", out, err) && out == r, "an empty path is the workspace itself");
    check(!resolve_in_workspace(r, "../outside/secret.txt", out, err) && err.find("outside") != std::string::npos,
          "`..` out of the workspace is refused");
    check(!resolve_in_workspace(r, "src/../../x", out, err), "`..` deeper in the path is refused too");
    check(resolve_in_workspace(r, "src/../src/main.cpp", out, err), "`..` that stays inside is fine");
    check(!resolve_in_workspace(r, (outside.path / "secret.txt").u8string(), out, err),
          "an absolute path outside is refused");
    check(resolve_in_workspace(r, (root.path / "src").u8string(), out, err), "an absolute path inside is fine");
    std::error_code ec;
    fs::create_directory_symlink(outside.path, root.path / "link_out", ec);
    fs::create_symlink(root.path / "src" / "main.cpp", root.path / "link_in", ec);
    fs::create_symlink(outside.path / "missing.txt", root.path / "dangling", ec);
    if (!ec) {
        check(!resolve_in_workspace(r, "link_out/secret.txt", out, err), "a link to a folder outside is refused");
        check(!resolve_in_workspace(r, "link_out/new.txt", out, err), "a new file through a link outside is refused");
        check(resolve_in_workspace(r, "link_in", out, err) && out == (root.path / "src" / "main.cpp").u8string(),
              "a link inside resolves to its target");
        check(!resolve_in_workspace(r, "dangling", out, err), "a dangling link is refused");
    } else {
        std::printf("(symbolic links not available here: link checks skipped)\n");
    }
    check(!resolve_in_workspace((root.path / "gone").u8string(), "x", out, err), "a workspace that is gone refuses");
    check(workspace_relative(r, (root.path / "src" / "main.cpp").u8string()) == "src/main.cpp",
          "a path is shown relative to the workspace");
}

static void classifier_tests() {
    for (const char * c : {"ls -la", "cat a.txt | grep -n foo | head -3", "find . -name '*.cpp'", "rg TODO src",
                           "wc -l a b", "pwd", "grep -rn \"a b\" .", "sort -r names.txt", "sort --reverse names.txt"})
        check(is_read_only_command(c), std::string("reads only: ") + c);
    for (const char * c : {"rm -rf build",
                           "ls > out.txt",
                           "cat a >> b",
                           "echo $(rm x)",
                           "echo `rm x`",
                           "ls; rm x",
                           "ls && rm x",
                           "ls & rm x",
                           "find . -delete",
                           "find . -exec rm {} +",
                           "git push",
                           "git -c core.pager=sh status",
                           "git",
                           "git status",
                           "git diff HEAD~1 -- src",
                           "git log --oneline -5",
                           "sort -o out in",
                           "sort -ro out in",
                           "sort --o=out in",
                           "sort --outp=out in",
                           "sort --compress-program=sh in",
                           "sort --co=sh in",
                           "date --s=2020",
                           "/bin/ls",
                           "FOO=1 ls",
                           "rg --pre=sh x",
                           "tree -o out",
                           "python script.py",
                           "cat 'unterminated",
                           "ls |",
                           "",
                           "git diff --output=x",
                           "date -s 2020",
                           "sed -n 1p a"})
        check(!is_read_only_command(c), std::string("may change something: ") + c);
    // A read that may leave the workspace asks, like a change.
    for (const char * c : {"cat ~/.ssh/id_rsa", "cat /etc/passwd", "ls ../other", "grep -r key C:\\Users",
                           "rg --ignore-file=/etc/x key", "head -c 10 ~"})
        check(!is_read_only_command(c), std::string("may read outside the workspace: ") + c);
}

static void diff_tests() {
    const std::string before = "a\nb\nc\nd\ne\nf\ng\nh\ni\n";
    const std::string after = "a\nb\nc\nd\nE\nf\ng\nh\ni\n";
    const std::string d = unified_diff("x.txt", before, after);
    check(d.find("--- a/x.txt\n+++ b/x.txt\n@@ -2,7 +2,7 @@\n b\n c\n d\n-e\n+E\n f\n g\n h\n") == 0,
          "an edit is one hunk with three lines of context");
    check(unified_diff("x", "same", "same").empty(), "no change, no diff");
    const std::string n = unified_diff("new.txt", "", "one\ntwo\n");
    check(n.find("--- /dev/null\n+++ b/new.txt\n@@ -0,0 +1,2 @@\n+one\n+two\n") == 0, "a new file diffs from nothing");
    const std::string nl = unified_diff("f", "x\n", "x");
    check(nl.find("-x\n+x\n\\ No newline at end of file") != std::string::npos, "a lost final newline is shown");
}

static std::string tool_text(const ToolOutput & o) {
    return o.text;
}

static void workspace_tool_tests() {
    check(glob_match("*.cpp", "a.cpp") && !glob_match("*.cpp", "src/a.cpp"), "glob: * stays inside one name");
    check(glob_match("**/*.cpp", "a.cpp") && glob_match("**/*.cpp", "src/x/a.cpp") &&
              glob_match("src/**/x.h", "src/x.h"),
          "glob: ** crosses folders, and **/ matches none");
    check(glob_match("[ab]?.txt", "a1.txt") && !glob_match("[!a]x", "ax") && glob_match("[a-c]", "b"),
          "glob: classes, negation and ranges");
    check(path_glob_match("*.md", "docs/a.md") && path_glob_match("docs/*.md", "x/docs/a.md") &&
              !path_glob_match("/docs/*.md", "x/docs/a.md") && path_glob_match("/docs/*.md", "docs/a.md"),
          "glob: a name pattern at any depth, a path pattern from any depth unless anchored");

    std::string after, err;
    check(
        apply_edits("one two three",
                    json::array({{{"old_text", "three"}, {"new_text", "3"}}, {{"old_text", "one"}, {"new_text", "1"}}}),
                    after, err) &&
            after == "1 two 3",
        "edit_file: several edits, matched against the original, applied at once");
    check(!apply_edits("a a", json::array({{{"old_text", "a"}, {"new_text", "b"}}}), after, err) &&
              err.find("2 occurrences of edits[0]") != std::string::npos,
          "edit_file: a passage found twice is refused, by its index");
    check(!apply_edits("abc", json::array({{{"old_text", "x"}, {"new_text", "y"}}}), after, err) &&
              err.find("could not find edits[0]") != std::string::npos,
          "edit_file: a passage not found is refused");
    check(!apply_edits("abcdef",
                       json::array({{{"old_text", "abc"}, {"new_text", "1"}}, {{"old_text", "cd"}, {"new_text", "2"}}}),
                       after, err) &&
              err.find("overlap") != std::string::npos,
          "edit_file: overlapping edits are refused");
    check(!apply_edits("abc", json::array({{{"old_text", "b"}, {"new_text", "b"}}}), after, err) &&
              err.find("no changes") != std::string::npos,
          "edit_file: an edit that changes nothing is refused");
    check(!apply_edits("abc", json::array(), after, err) &&
              !apply_edits("abc", json::array({{{"old_text", ""}, {"new_text", "b"}}}), after, err),
          "edit_file: no edits, or an empty old_text, is refused");

    TempDir ws("tools");
    const std::string w = ws.path.u8string();
    spit(ws.path / "src" / "a.cpp", "int a;\nint b;\n// TODO fix\n");
    spit(ws.path / "src" / "deep" / "b.cpp", "int TODO_x;\n");
    spit(ws.path / "README.md", "line one\nline two\nline three\n");
    spit(ws.path / "node_modules" / "dep" / "x.js", "TODO\n");
    spit(ws.path / "bin.dat", std::string("a\0b", 3));
    std::string t =
        tool_text(read_file(w, {{"path", "README.md"}, {"start_line", 2}, {"end_line", 3}, {"append_loc", true}}));
    check(t == "2\xE2\x86\x92line two\n3\xE2\x86\x92line three\n",
          "read_file: a line range, numbered as N\xE2\x86\x92");
    check(!read_file(w, {{"path", "bin.dat"}}).ok && !read_file(w, {{"path", "src"}}).ok &&
              !read_file(w, {{"path", "../x"}}).ok,
          "read_file: a binary file, a folder and a path outside are refused");
    spit(ws.path / "big.txt", std::string(k_tool_result_max_bytes, 'x') + "\nnext\n");
    t = tool_text(read_file(w, {{"path", "big.txt"}}));
    check(t.find("[output truncated at line 1 of 2: read on with start_line=1]") != std::string::npos,
          "read_file: a file past the cap says where to read on");

    t = tool_text(file_glob_search(w, {{"path", "."}, {"include", "*.cpp"}}));
    check(t.find("src/a.cpp\nsrc/deep/b.cpp\n") == 0 && t.find("Total matches: 2") != std::string::npos,
          "file_glob_search: files by name at any depth, in order, with the total");
    t = tool_text(file_glob_search(w, {{"path", "."}, {"include", "*.js"}}));
    check(t.find("x.js") == std::string::npos, "file_glob_search: junk folders are not entered");
    t = tool_text(file_glob_search(w, {{"path", "."}, {"type", "dir"}, {"max_depth", 1}}));
    check(t.find("node_modules/\n") != std::string::npos && t.find("src/\n") != std::string::npos &&
              t.find("src/deep/") == std::string::npos,
          "file_glob_search: folders with type dir, one level with max_depth 1");
    t = tool_text(file_glob_search(w, {{"path", "src"}, {"limit", 1}}));
    check(t.find("a.cpp\n") == 0 && t.find("1 results limit reached (2 total matches)") != std::string::npos,
          "file_glob_search: paths relative to the folder searched, and the limit said");
    check(!file_glob_search(w, {{"path", "."}, {"type", "x"}}).ok && !file_glob_search(w, {{"path", ".."}}).ok,
          "file_glob_search: a bad type and a folder outside are refused");

    t = tool_text(grep_search(w, {{"path", "."}, {"pattern", "TODO"}, {"return_line_numbers", true}}));
    check(t.find("src/a.cpp:3:// TODO fix\n") != std::string::npos &&
              t.find("src/deep/b.cpp:1:") != std::string::npos && t.find("x.js") == std::string::npos &&
              t.find("Total matches: 2") != std::string::npos,
          "grep_search: path:line:text, junk folders skipped");
    t = tool_text(grep_search(w, {{"path", "src"}, {"pattern", "int [ab];"}, {"include", "a.cpp"}}));
    check(t.find("a.cpp:int a;\na.cpp:int b;\n") == 0, "grep_search: a regular expression, include by name");
    t = tool_text(
        grep_search(w, {{"path", "README.md"}, {"pattern", "LINE TWO"}, {"ignore_case", true}, {"context_lines", 1}}));
    check(t.find("README.md-1-line one\nREADME.md:2:line two\nREADME.md-3-line three\n--\n") == 0,
          "grep_search: one file, ignoring case, with context lines");
    t = tool_text(grep_search(w, {{"path", "src"}, {"pattern", "int a;"}, {"literal", true}}));
    check(t.find("Total matches: 1") != std::string::npos, "grep_search: a literal pattern");
    check(!grep_search(w, {{"path", "."}, {"pattern", "("}}).ok &&
              !grep_search(w, {{"path", ".."}, {"pattern", "x"}}).ok,
          "grep_search: a bad expression and a folder outside are refused");

    const std::string capped = cap_text(std::string(10, 'a') + "\xC3\xA9", 11);
    check(capped.compare(0, 11, std::string(10, 'a') + "\n") == 0 && capped.find("2 more bytes") != std::string::npos,
          "a cap never splits a character and says what it left out");
}

static void skill_tests() {
    Skill s;
    std::string err;
    check(parse_skill("---\nname: my-skill\ndescription: Does things: well.\n---\n\nBody here.\n", s, err) &&
              s.name == "my-skill" && s.description == "Does things: well." && s.body == "Body here." &&
              s.title == "My skill",
          "front matter and body are read; the title defaults to the name in words");
    check(
        parse_skill("---\r\nname: a\r\ndescription: >\r\n  folded\r\n  text\r\ntitle: \"T: x\"\r\n---\r\nB", s, err) &&
            s.description == "folded text" && s.title == "T: x",
        "CRLF, a folded block and a quoted value are read");
    check(!parse_skill("name: x\n", s, err), "no front matter is refused");
    check(!parse_skill("---\nname: Bad_Name\ndescription: d\n---\n", s, err),
          "a name that is not kebab-case is refused");
    check(!parse_skill("---\nname: ok\n---\nbody", s, err), "a missing description is refused");
    check(!parse_skill("---\nname: ok\ndescription: d\n", s, err), "an unclosed front matter is refused");
    check(valid_skill_name("a-b-1") && !valid_skill_name("-a") && !valid_skill_name("a--b") &&
              !valid_skill_name("../x"),
          "names are kebab-case and can never be a path");
    Skill w;
    w.name = "round-trip";
    w.title = "Round \"trip\"";
    w.description = "Has \"quotes\", a: colon and \\ backslash";
    w.body = "Line 1\nLine 2";
    Skill back;
    check(parse_skill(render_skill(w), back, err) && back.name == w.name && back.description == w.description &&
              back.body == w.body,
          "a written skill reads back the same");
    TempDir dir("skills");
    spit(dir.path / "good" / "SKILL.md", "---\nname: good\ndescription: d\n---\nok");
    spit(dir.path / "bad" / "SKILL.md", "no front matter");
    spit(dir.path / "other" / "SKILL.md", "---\nname: elsewhere\ndescription: d\n---\n");
    std::vector<std::string> warnings;
    const std::vector<Skill> found = scan_skills(dir.path.u8string(), "user", warnings);
    check(found.size() == 1 && found[0].name == "good" && found[0].source == "user" && warnings.size() == 2,
          "a folder of skills: bad files and a name that is not its folder's are warned about and skipped");
}

static void set_tz(const char * tz) {
#if defined(_WIN32)
    _putenv_s("TZ", tz);
    _tzset();
#else
    setenv("TZ", tz, 1);
    tzset();
#endif
}

static void schedule_tests() {
    set_tz("UTC");
    const std::time_t day = 86400;
    const std::time_t monday_10 = 1793008800; // 2026-10-26 10:00 UTC, a Monday
    Schedule s;
    std::string err;
    check(parse_schedule(json{{"kind", "daily"}, {"at", "09:30"}}, s, err), "a daily schedule parses");
    check(next_run(s, monday_10, true) == monday_10 - 1800 + day, "daily: the time passed today, so tomorrow");
    check(parse_schedule(json{{"kind", "daily"}, {"at", "11:00"}}, s, err) &&
              next_run(s, monday_10, false) == monday_10 + 3600,
          "daily: still today when the time is ahead");
    check(parse_schedule(json{{"kind", "weekly"}, {"at", "10:00"}, {"weekdays", {3, 5}}}, s, err) &&
              next_run(s, monday_10, true) == monday_10 + 2 * day,
          "weekly: the next chosen weekday (Wednesday)");
    check(parse_schedule(json{{"kind", "weekly"}, {"at", "10:00"}, {"weekdays", {1}}}, s, err) &&
              next_run(s, monday_10, true) == monday_10 + 7 * day,
          "weekly: the same weekday at the same time is next week");
    check(parse_schedule(json{{"kind", "interval"}, {"every_minutes", 30}}, s, err) &&
              next_run(s, monday_10, true) == monday_10 + 1800,
          "interval: every so many minutes from the last run");
    check(parse_schedule(json{{"kind", "once"}, {"at", "2026-10-26T12:00"}}, s, err) &&
              next_run(s, monday_10, false) == monday_10 + 7200 && next_run(s, monday_10, true) == 0,
          "once: its time, and never again after it ran");
    check(parse_schedule(json{{"kind", "once"}, {"at", "2026-10-26T08:00"}}, s, err) &&
              next_run(s, monday_10, false) == monday_10 - 7200,
          "once: a time that passed while nothing ran is still due");
    check(!parse_schedule(json{{"kind", "interval"}, {"every_minutes", 1}}, s, err),
          "too short an interval is refused");
    check(!parse_schedule(json{{"kind", "daily"}, {"at", "25:00"}}, s, err),
          "a time of day that does not exist is refused");
    check(!parse_schedule(json{{"kind", "weekly"}, {"at", "10:00"}}, s, err), "weekly without a weekday is refused");
    check(!parse_schedule(json{{"kind", "hourly"}}, s, err), "an unknown kind is refused");
    set_tz("Europe/Rome");
    check(parse_schedule(json{{"kind", "daily"}, {"at", "09:00"}}, s, err) &&
              next_run(s, 1792735200 /* 2026-10-23 08:00 Rome, CEST */, true) == 1792735200 + 3600,
          "daily in a zone with daylight saving: local time of day");
    check(next_run(s, 1792825200 /* 2026-10-24 09:00 CEST */, true) == 1792825200 + day + 3600,
          "across the change to winter time the day is 25 hours");
    set_tz("UTC");
}

static void provider_tests() {
    ChatRequest req;
    req.messages = {{"system", "s"}, {"user", "u"}};
    ChatMessage a{"assistant", ""};
    a.tool_calls.push_back({"c1", "read_file", "{\"path\":\"a\"}"});
    req.messages.push_back(a);
    ChatMessage t{"tool", "contents"};
    t.tool_call_id = "c1";
    t.name = "read_file";
    req.messages.push_back(t);
    req.tools.push_back({"read_file", "Read", "{\"type\":\"object\"}"});
    req.n_predict = 100;
    const json body = provider_request_json(req, "gpt-x", true);
    check(body["model"] == "gpt-x" && body["stream"] == true && body["max_tokens"] == 100, "model, stream and budget");
    check(body["messages"][2]["tool_calls"][0]["function"]["name"] == "read_file" &&
              body["messages"][3]["tool_call_id"] == "c1" &&
              body["tools"][0]["function"]["parameters"]["type"] == "object",
          "calls, answers and tools in the OpenAI shape");
    ProviderStream ps;
    const std::string sse = "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"think\"}}]}\n\n"
                            "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n"
                            "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
                            ": keep-alive\n\n"
                            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"x\",\"function\":"
                            "{\"name\":\"read_file\",\"arguments\":\"{\\\"pa\"}}]}}]}\n\n"
                            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":"
                            "\"th\\\":1}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
                            "data: [DONE]\n\n";
    for (size_t i = 0; i < sse.size(); i += 7)
        ps.feed(sse.data() + i, std::min<size_t>(7, sse.size() - i));
    ps.finish_body(sse);
    check(ps.text() == "Hello" && ps.reasoning() == "think" && ps.done() && ps.finish_reason() == "tool_calls",
          "a stream fed in pieces of any size folds into text and reasoning");
    const auto calls = ps.tool_calls();
    check(calls.size() == 1 && calls[0].id == "x" && calls[0].arguments == "{\"path\":1}",
          "a tool call's arguments are joined across chunks");
    ProviderStream err;
    err.finish_body("{\"error\":{\"message\":\"bad key\"}}");
    check(err.error() == "bad key", "an error body says why");
    check(provider_error_message("<html>oops</html>") == "<html>oops</html>", "a body that is not JSON is quoted");
}

static void connect_tests() {
    std::string out, err;
    const std::string existing = "model = \"mine\"\n\n[profiles.other]\nmodel = \"x\"\n";
    check(merge_settings("toml_block", existing, "[profiles.bmoe]\nmodel = \"m\"\n", out, err) &&
              out.find(existing) == 0 && out.find("[profiles.bmoe]") != std::string::npos,
          "toml: the person's settings stay, the block is added at the end");
    std::string twice;
    check(merge_settings("toml_block", out, "[profiles.bmoe]\nmodel = \"n\"\n", twice, err) &&
              twice.find("model = \"m\"") == std::string::npos && twice.find("model = \"n\"") != std::string::npos &&
              twice.find(existing) == 0,
          "toml: a second connect replaces its own block, not the person's");
    check(merge_settings("json_merge", "{\"theme\":\"dark\",\"provider\":{\"other\":{}}}",
                         "{\"provider\":{\"bmoe\":{\"name\":\"B\"}}}", out, err) &&
              json::parse(out)["theme"] == "dark" && json::parse(out)["provider"].contains("other") &&
              json::parse(out)["provider"]["bmoe"]["name"] == "B",
          "json: the snippet is merged key by key");
    check(!merge_settings("json_merge", "{ // a comment\n}", "{}", out, err),
          "json: a file that does not parse is not overwritten");
    ConnectFacts f{"http://127.0.0.1:8765", true, "qwen \"3\"", ""};
    check(fill_template("{{base_url}} {{api_key}} {{model}}", f, true) ==
              "http://127.0.0.1:8765/v1 <this server's API key> qwen \\\"3\\\"",
          "placeholders filled, the key itself never");
    TempDir home("home");
    f.home = home.path.u8string();
    f.model = "m";
    const json catalog = json{{"clients",
                               {{{"id", "c"},
                                 {"name", "C"},
                                 {"how", "h"},
                                 {"file", "~/.c/config.toml"},
                                 {"write", "toml_block"},
                                 {"snippet", "[x]\nm = \"{{model}}\"\n"}},
                                {{"id", "s"}, {"name", "S"}, {"snippet", "only a snippet"}}}}};
    const json list = connect_list(catalog, f);
    check(list.size() == 2 && list[0]["writable"] == true && list[1]["writable"] == false &&
              list[0]["file"] == (home.path / ".c" / "config.toml").u8string(),
          "the list says which clients the server can write, with the file");
    spit(home.path / ".c" / "config.toml", "a = 1\n");
    json result;
    int status = 0;
    check(connect_write(catalog, "c", f, result, status, err), "a client's settings are written");
    const std::string backup = result.value("backup", "");
    check(!backup.empty() && slurp(fs::u8path(backup)) == "a = 1\n", "the file it replaced is kept as a backup");
    check(slurp(home.path / ".c" / "config.toml").find("m = \"m\"") != std::string::npos, "the new settings are there");
    check(!connect_write(catalog, "s", f, result, status, err) && status == 400,
          "a snippet-only client is not written");
    f.model.clear();
    check(!connect_write(catalog, "c", f, result, status, err) && status == 409, "no model loaded: nothing is written");
}

static void store_tests() {
    TempDir data("store");
    {
        AgentStore st(data.path.u8string());
        st.load();
        std::string err;
        json p{{"id", "cloud"}, {"name", "Cloud"}, {"base_url", "https://x/v1"}, {"models", {"m"}}};
        const std::string key = "sk-very-secret";
        check(st.put_provider(p, &key, err), "a provider with a key is saved");
        const std::string listing = st.providers().dump();
        check(listing.find(key) == std::string::npos && st.providers()[0]["has_key"] == true,
              "a listing says a key is there and never shows it");
        check(slurp(data.path / "agent" / "providers.json").find(key) == std::string::npos,
              "the providers file does not hold the key");
        check(slurp(data.path / "credentials.json").find(key) != std::string::npos, "the credentials file does");
#if !defined(_WIN32)
        const fs::perms perms = fs::status(data.path / "credentials.json").permissions();
        check((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
              "the credentials file is readable by its owner only");
#endif
        json got;
        std::string k;
        check(st.provider("cloud", got, k) && k == key, "the run loop can read the key");
        check(st.put_provider(p, nullptr, err) && st.provider("cloud", got, k) && k == key,
              "a change without a key keeps it");
        const json c = st.create_conversation(json{{"title", "t"}});
        st.edit_conversation(c.value("id", ""), [](json & cv) { cv["state"] = "running"; });
        check(st.remove_provider("cloud") && slurp(data.path / "credentials.json").find(key) == std::string::npos,
              "forgetting a provider forgets its key");
    }
    {
        AgentStore again(data.path.u8string());
        again.load();
        const json list = again.list_conversations();
        check(list.size() == 1 && list[0].value("state", "") == "idle", "a run the server stopped during reads idle");
        json c;
        again.conversation(list[0].value("id", ""), c);
        check(c["items"].size() == 1 && c["items"][0].value("kind", "") == "notice", "and says so in a notice");
        spit(data.path / "agent" / "tasks.json", "{ not json");
        const std::vector<std::string> warnings = again.load();
        check(!warnings.empty() && fs::exists(data.path / "agent" / "tasks.json.bad"),
              "a file that does not parse is set aside, not a failed start");
        json w;
        std::string err;
        check(!again.add_workspace("relative/path", "", w, err), "a workspace must be an absolute folder");
        check(again.add_workspace(data.path.u8string(), "", w, err) && again.workspaces().size() == 1,
              "a folder is added");
        json w2;
        check(again.add_workspace(data.path.u8string() + "/.", "", w2, err) && w2["id"] == w["id"] &&
                  again.workspaces().size() == 1,
              "the same folder twice is the same workspace");
    }
}

static void command_tests() {
    if (!platform::can_run_commands()) {
        std::printf("(this platform starts no command: run_command checks skipped)\n");
        return;
    }
    TempDir dir("cmd");
    std::atomic<bool> stop{false};
#if defined(_WIN32)
    const std::string hello = "echo hi& exit 3", slow = "ping -n 30 127.0.0.1 >NUL", where = "cd";
#else
    const std::string hello = "echo hi; exit 3", slow = "sleep 30", where = "pwd";
#endif
    platform::CommandResult r = platform::run_command(hello, dir.path.u8string(), 10, 1024, stop);
    check(r.started && r.exit_code == 3 && r.output.find("hi") == 0, "a command runs, with its output and exit code");
    r = platform::run_command(where, dir.path.u8string(), 10, 1024, stop);
    check(r.output.find(dir.path.filename().u8string()) != std::string::npos, "it runs in the workspace folder");
    auto t0 = std::chrono::steady_clock::now();
    r = platform::run_command(slow, dir.path.u8string(), 1, 1024, stop);
    check(r.timed_out && std::chrono::steady_clock::now() - t0 < 5s, "a command past its time limit is ended");
    std::thread raiser([&] {
        std::this_thread::sleep_for(200ms);
        stop = true;
    });
    t0 = std::chrono::steady_clock::now();
    r = platform::run_command(slow, dir.path.u8string(), 30, 1024, stop);
    raiser.join();
    check(r.stopped && std::chrono::steady_clock::now() - t0 < 5s, "a stop ends a running command");
    stop = false;
#if !defined(_WIN32)
    t0 = std::chrono::steady_clock::now();
    r = platform::run_command("sleep 30 & echo started", dir.path.u8string(), 10, 1024, stop);
    check(r.exit_code == 0 && r.output.find("started") == 0 && std::chrono::steady_clock::now() - t0 < 5s,
          "a background child holding the output does not hold the tool call");
    r = platform::run_command("yes | head -c 100000", dir.path.u8string(), 10, 1000, stop);
    check(r.output.size() == 1000 && r.truncated, "output past the cap is dropped and said to be");

    // A program spoken to a line at a time, as another agent is: cat says back what it is told.
    check(!platform::find_program("cat").empty() && platform::find_program("no-such-program-here").empty(),
          "a program is found on PATH, and one that is not there is not");
    std::string err;
    auto cat = platform::LineProcess::start({"cat"}, dir.path.u8string(), err);
    check(cat != nullptr, "a line process starts");
    std::string line;
    check(cat && cat->write_line("{\"jsonrpc\":\"2.0\"}") && cat->read_line(line) && line == "{\"jsonrpc\":\"2.0\"}",
          "a line goes in and comes back whole");
    if (cat) cat->stop();
    check(cat && !cat->read_line(line) && !cat->write_line("x"), "after stop() it reads its end and takes no more");
    check(platform::LineProcess::start({"no-such-program-here"}, "", err) == nullptr &&
              err.find("not installed") != std::string::npos,
          "a program that is not installed says so");
#endif
}

static void access_tests() {
    check(agent_route_locality("/api/workspaces") == Locality::Always &&
              agent_route_locality("/api/workspaces/w1") == Locality::Always,
          "workspaces: listing, adding and removing are local only (a workspace names a folder here)");
    check(agent_route_locality("/api/conversations/c1/approval") == Locality::Always, "an approval is local only");
    for (const char * p : {"/api/conversations/c1/messages", "/api/conversations/c1/edit", "/api/conversations/c1/file",
                           "/api/conversations/c1", "/api/conversations/c1/stop"})
        check(agent_route_locality(p) == Locality::IfConversationReaches,
              std::string("depends on the conversation: ") + p);
    check(agent_route_locality("/api/conversations") == Locality::Anywhere, "plain chat works from the network");

    // What reaches this computer, and what a client on the network may not set.
    check(!conversation_reaches(json{{"workspace", nullptr}, {"route", "local"}}) &&
              conversation_reaches(json{{"workspace", "w1"}, {"route", "local"}}) &&
              conversation_reaches(json{{"workspace", nullptr}, {"route", "p1/m"}}),
          "a conversation reaches this computer through a workspace or a saved provider");
    for (const json & f : {json{{"workspace", "w1"}}, json{{"route", "p1/m"}}, json{{"mode", "full"}}})
        check(network_may_not_set(f), "the network may not set " + f.dump());
    for (const json & f : {json::object(), json{{"workspace", nullptr}, {"route", "local"}, {"mode", "ask"}},
                           json{{"mode", "read_only"}, {"title", "x"}}})
        check(!network_may_not_set(f), "the network may set " + f.dump());
    // Whatever the method: reading them is as local as changing them.
    for (const char * p : {"/api/skills", "/api/skills/x", "/api/tasks", "/api/tasks/t/run", "/api/providers",
                           "/api/providers/p", "/api/connect", "/api/connect/codex"})
        check(agent_route_locality(p) == Locality::Always, std::string("local only: ") + p);
    check(agent_route_locality("/api/skillsx") == Locality::Anywhere, "a prefix is matched on a path boundary");
}

// An event for local clients only never reaches a stream from the network.
static void audience_tests() {
    EventBus bus;
    auto here = bus.subscribe(true);
    auto away = bus.subscribe(false);
    bus.publish("item", json{{"secret", 1}}, EventBus::Audience::LocalOnly);
    bus.publish("conversation", json{{"plain", 1}});
    const auto frames = [](EventBus::Queue & q) {
        std::string all, f;
        while (EventBus::next(q, f, 20ms) && !f.empty())
            all += f;
        return all;
    };
    const std::string h = frames(*here), a = frames(*away);
    check(h.find("secret") != std::string::npos && h.find("plain") != std::string::npos,
          "a local stream gets every event");
    check(a.find("secret") == std::string::npos && a.find("plain") != std::string::npos,
          "a stream from the network gets only the events for everyone");
    check(conversation_audience(json{{"workspace", "w1"}}) == EventBus::Audience::LocalOnly &&
              conversation_audience(json{{"workspace", nullptr}, {"route", "local"}}) == EventBus::Audience::Everyone,
          "a conversation that reaches this computer is shown to local clients only");
    bus.unsubscribe(here);
    bus.unsubscribe(away);
}

// ---------------------------------------------------------------------------------------------
// Another agent over the Agent Client Protocol: an in-process stand-in that answers as `behave`
// says, so the bridge is tested without a program or a model.

class FakeAcpAgent : public JsonRpcTransport {
public:
    using Behave = std::function<void(FakeAcpAgent &, const json & message)>;
    explicit FakeAcpAgent(Behave b) : behave_(std::move(b)) {
        thread_ = std::thread([this] {
            json m;
            while (next(m))
                if (m.contains("method")) behave_(*this, m);
        });
    }
    ~FakeAcpAgent() override {
        stop();
        if (thread_.joinable()) thread_.join();
    }
    bool write_line(const std::string & line) override {
        if (line.find('\n') != std::string::npos) wire_ok = false;
        // Seen as it arrives: the agent's own thread may be busy answering the prompt it cancels.
        if (line.find("\"session/cancel\"") != std::string::npos) cancelled = true;
        std::lock_guard<std::mutex> lk(m_);
        if (closed_) return false;
        in_.push_back(line);
        cv_.notify_all();
        return true;
    }
    bool read_line(std::string & line) override {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return closed_ || !out_.empty(); });
        if (out_.empty()) return false;
        line = out_.front();
        out_.pop_front();
        return true;
    }
    void stop() override {
        std::lock_guard<std::mutex> lk(m_);
        closed_ = true;
        cv_.notify_all();
    }
    // To the client.
    void send(const json & m) {
        std::lock_guard<std::mutex> lk(m_);
        out_.push_back(m.dump());
        cv_.notify_all();
    }
    void reply(const json & request, const json & result) {
        send({{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", result}});
    }
    void update(const json & u) {
        send({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", {{"sessionId", "s1"}, {"update", u}}}});
    }
    // A request to the client, and its answer (the messages in between go to `behave`).
    json ask(const std::string & method, const json & params) {
        const int id = ++asked_;
        send({{"jsonrpc", "2.0"}, {"id", 1000 + id}, {"method", method}, {"params", params}});
        json m;
        while (next(m)) {
            if (m.contains("method")) {
                behave_(*this, m);
                continue;
            }
            if (m.value("id", 0) == 1000 + id) return m;
        }
        return json();
    }
    std::atomic<bool> wire_ok{true};
    std::atomic<bool> cancelled{false};

private:
    bool next(json & m) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return closed_ || !in_.empty(); });
        if (in_.empty()) return false;
        m = json::parse(in_.front());
        in_.pop_front();
        return true;
    }
    Behave behave_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::string> in_, out_;
    bool closed_ = false;
    int asked_ = 0;
    std::thread thread_;
};

// A rig whose conversations may use "fake", an agent that behaves as `b`.
static std::unique_ptr<Rig> acp_rig(FakeAcpAgent::Behave b, FakeAcpAgent ** last = nullptr) {
    return std::make_unique<Rig>(true, true, [b, last](AgentDeps & d) {
        d.external_agents = json::array(
            {{{"id", "fake"}, {"name", "Fake"}, {"acp", {{"command", {"fake-acp"}}, {"needs", {"fake-acp"}}}}},
             {{"id", "absent"}, {"name", "Absent"}, {"acp", {{"command", {"nope"}}, {"needs", {"nope"}}}}}});
        d.program_found = [](const std::string & p) { return p == "fake-acp"; };
        d.start_agent = [b, last](const std::vector<std::string> &, const std::string &, std::string &) {
            auto a = std::make_unique<FakeAcpAgent>(b);
            if (last) *last = a.get();
            return std::unique_ptr<JsonRpcTransport>(std::move(a));
        };
    });
}

// The handshake every fake agent answers the same way.
static bool handshake(FakeAcpAgent & a, const json & m) {
    const std::string method = m.value("method", "");
    if (method == "initialize") {
        a.reply(m, {{"protocolVersion", 1}, {"agentCapabilities", {{"loadSession", false}}}});
        return true;
    }
    if (method == "session/new") {
        a.reply(m, {{"sessionId", "s1"}});
        return true;
    }
    return false;
}

static void external_agent_tests() {
    {
        auto rig = acp_rig([](FakeAcpAgent &, const json &) {});
        const json caps = rig->agent->capabilities(true);
        check(caps["agents"].size() == 3 && caps["agents"][0].value("id", "") == "builtin" &&
                  caps["agents"][1].value("available", false) && !caps["agents"][2].value("available", true) &&
                  caps["agents"][2]["missing"][0] == "nope",
              "acp: the built-in agent and each other one, with whether its program is installed");
        check(rig->agent->knows_agent("fake") && !rig->agent->knows_agent("other"), "acp: agents are the catalog's");
    }
    {
        std::string ws_path;
        json write_answer, outside_answer;
        auto rig = acp_rig([&](FakeAcpAgent & a, const json & m) {
            if (handshake(a, m)) return;
            if (m.value("method", "") != "session/prompt") return;
            const std::string file = (fs::u8path(ws_path) / "a.txt").u8string();
            a.update({{"sessionUpdate", "agent_message_chunk"}, {"content", {{"type", "text"}, {"text", "Working"}}}});
            a.update({{"sessionUpdate", "tool_call"},
                      {"toolCallId", "t1"},
                      {"title", "Write a.txt"},
                      {"kind", "edit"},
                      {"status", "pending"},
                      {"content", {{{"type", "diff"}, {"path", file}, {"oldText", nullptr}, {"newText", "hi\n"}}}}});
            const json perm = a.ask("session/request_permission",
                                    {{"sessionId", "s1"},
                                     {"toolCall", {{"toolCallId", "t1"}}},
                                     {"options",
                                      {{{"optionId", "yes"}, {"name", "Allow"}, {"kind", "allow_once"}},
                                       {{"optionId", "no"}, {"name", "Reject"}, {"kind", "reject_once"}}}}});
            if (perm["result"]["outcome"].value("optionId", "") == "yes")
                write_answer = a.ask("fs/write_text_file", {{"sessionId", "s1"}, {"path", file}, {"content", "hi\n"}});
            outside_answer = a.ask("fs/read_text_file", {{"sessionId", "s1"}, {"path", "/etc/hosts"}});
            a.update({{"sessionUpdate", "tool_call_update"}, {"toolCallId", "t1"}, {"status", "completed"}});
            a.update({{"sessionUpdate", "agent_message_chunk"}, {"content", {{"type", "text"}, {"text", "Done."}}}});
            a.reply(m, {{"stopReason", "end_turn"}});
        });
        ws_path = rig->ws.path.u8string();
        const std::string c =
            rig->store->create_conversation({{"mode", "ask"}, {"workspace", rig->workspace_id}, {"agent", "fake"}})
                .value("id", "");
        json item;
        check(rig->agent->send(c, "write a file", false, false, item).ok(), "acp: a message starts the other agent");
        check(rig->wait_state(c, "waiting"), "acp: its permission request is an approval card");
        const json card = rig->items_of(c, "tool")[0];
        check(card.value("diff", "").find("+hi") != std::string::npos && card.value("file", "") == "a.txt",
              "acp: the card carries the agent's diff, relative to the workspace");
        check(!fs::exists(rig->ws.path / "a.txt"), "acp: nothing is written before the yes");
        check(rig->agent->approve(c, rig->waiting_item(c), true).ok(), "acp: the approval is taken");
        check(rig->wait_done(c), "acp: the turn ends");
        check(slurp(rig->ws.path / "a.txt") == "hi\n" && write_answer.contains("result"),
              "acp: the agent's write lands in the workspace");
        check(outside_answer.contains("error") && !outside_answer.contains("result"),
              "acp: a read outside the workspace is refused");
        const json said = rig->items_of(c, "assistant");
        check(said.size() == 2 && said[0].value("text", "") == "Working" && said[1].value("text", "") == "Done." &&
                  !said[1].value("streaming", true),
              "acp: the agent's text, split around its tool call, as assistant items");
        check(rig->items_of(c, "tool")[0].value("state", "") == "done", "acp: the tool item ends done");
        check(rig->conv(c).value("acp_session", "") == "s1", "acp: the session is kept for the next message");
        json again;
        check(rig->agent->edit(c, item.value("id", ""), "other", false, false, again).status == 409,
              "acp: another agent's session is not rewritten by an edit");
    }
    {
        FakeAcpAgent * fake = nullptr;
        auto rig = acp_rig(
            [](FakeAcpAgent & a, const json & m) {
                if (handshake(a, m)) return;
                // A prompt is answered once it is cancelled, as an agent that was told to stop.
                if (m.value("method", "") == "session/prompt") {
                    a.update(
                        {{"sessionUpdate", "agent_message_chunk"}, {"content", {{"type", "text"}, {"text", "..."}}}});
                    for (int i = 0; i < 2000 && !a.cancelled; ++i)
                        std::this_thread::sleep_for(5ms);
                    a.reply(m, {{"stopReason", "cancelled"}});
                }
            },
            &fake);
        const std::string c =
            rig->store->create_conversation({{"mode", "full"}, {"workspace", rig->workspace_id}, {"agent", "fake"}})
                .value("id", "");
        json item;
        rig->agent->send(c, "go on forever", false, false, item);
        for (int i = 0; i < 500 && rig->items_of(c, "assistant").empty(); ++i)
            std::this_thread::sleep_for(2ms);
        check(rig->agent->stop(c), "acp: a stop reaches the other agent");
        check(fake && fake->cancelled && fake->wire_ok, "acp: as session/cancel, one message a line");
        const json notices = rig->items_of(c, "notice");
        check(!notices.empty() && notices.back().value("text", "") == "Stopped.", "acp: the turn reads stopped");
    }
}

// ---------------------------------------------------------------------------------------------
// MCP servers: FakeAcpAgent's JSON-RPC peer, answering as an MCP server.

struct FakeMcp {
    std::atomic<int> starts{0};
    std::atomic<bool> initialized{false};
    std::string protocol;
    std::mutex m;
};

// A server "srv" with tools `echo` (its text back), `boom` (an error) and `slow` (no answer), listed
// over two pages; with `with_read`, a server "read" whose tool `file` would be named read_file.
static McpHub::StartFn fake_mcp_start(FakeMcp & state) {
    return [&state](const McpServerConfig & c, std::string & err) -> std::unique_ptr<JsonRpcTransport> {
        if (c.name == "broken") {
            err = "no such program";
            return nullptr;
        }
        ++state.starts;
        const std::string name = c.name;
        return std::make_unique<FakeAcpAgent>([&state, name](FakeAcpAgent & a, const json & m) {
            const std::string method = m.value("method", "");
            const json p = m.value("params", json::object());
            if (method == "initialize") {
                {
                    std::lock_guard<std::mutex> lk(state.m);
                    state.protocol = p.value("protocolVersion", "");
                }
                a.reply(m,
                        {{"protocolVersion", k_mcp_protocol_version}, {"capabilities", {{"tools", json::object()}}}});
            } else if (method == "notifications/initialized") {
                state.initialized = true;
            } else if (method == "tools/list" && name == "read") {
                a.reply(m, {{"tools",
                             {{{"name", "file"}, {"description", "shadow"}, {"inputSchema", {{"type", "object"}}}}}}});
            } else if (method == "tools/list" && !p.contains("cursor")) {
                a.reply(m,
                        {{"tools",
                          {{{"name", "echo"},
                            {"description", "Say it back"},
                            {"inputSchema", {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}}}}}},
                         {"nextCursor", "p2"}});
            } else if (method == "tools/list") {
                a.reply(m, {{"tools", {{{"name", "boom"}}, {{"name", "slow"}}}}});
            } else if (method == "tools/call") {
                const std::string tool = p.value("name", "");
                if (tool == "echo")
                    a.reply(m, {{"content",
                                 {{{"type", "text"}, {"text", p["arguments"].value("text", "")}},
                                  {{"type", "image"}, {"data", "x"}}}}});
                else if (tool == "boom")
                    a.reply(m, {{"content", {{{"type", "text"}, {"text", "it broke"}}}}, {"isError", true}});
                // slow: never answers
            }
        });
    };
}

static std::vector<McpServerConfig> mcp_servers(std::initializer_list<const char *> names, int timeout_ms = 2000) {
    std::vector<McpServerConfig> out;
    for (const char * n : names) {
        McpServerConfig c;
        c.name = n;
        c.command = {"fake"};
        c.timeout_ms = timeout_ms;
        out.push_back(c);
    }
    return out;
}

static void mcp_tests() {
    {
        std::vector<std::string> w;
        const auto cfg =
            parse_mcp_config(json::parse(R"({"mcpServers":{"fs":{"command":"npx","args":["-y","pkg"],"env":{"K":"v"}},
                            "web":{"url":"https://x"},"none":{}}})"),
                             w);
        check(cfg.size() == 1 && cfg[0].name == "fs" &&
                  cfg[0].command == std::vector<std::string>{"npx", "-y", "pkg"} && cfg[0].env.at("K") == "v" &&
                  cfg[0].timeout_ms == k_mcp_call_timeout_ms,
              "mcp config: command, args and env, in the mcpServers format");
        check(w.size() == 2 && w[0].find("remote") != std::string::npos, "mcp config: a remote or empty entry is said");
    }
    FakeMcp state;
    McpHub hub(mcp_servers({"srv", "broken"}, 300), fake_mcp_start(state));
    check(state.starts == 0, "mcp: nothing starts before its tools are wanted");
    const std::vector<McpTool> tools = hub.tools();
    check(tools.size() == 3 && tools[0].exposed_name() == "srv_echo" && tools[2].exposed_name() == "srv_slow" &&
              tools[0].input_schema.value("type", "") == "object",
          "mcp: the tools of every page, named <server>_<tool>");
    {
        std::lock_guard<std::mutex> lk(state.m);
        check(state.protocol == k_mcp_protocol_version && state.initialized,
              "mcp: initialize with the protocol version, then notifications/initialized");
    }
    std::string text;
    check(hub.call("srv", "echo", {{"text", "hello"}}, text) && text == "hello\n[image content not shown]",
          "mcp: a call's text content comes back, other content named");
    check(!hub.call("srv", "boom", json::object(), text) && text == "it broke", "mcp: isError is a failed call");
    const auto t0 = std::chrono::steady_clock::now();
    check(!hub.call("srv", "slow", json::object(), text) && text.find("did not answer") != std::string::npos &&
              std::chrono::steady_clock::now() - t0 < 3s,
          "mcp: a call past its time limit fails");
    check(hub.call("srv", "echo", {{"text", "again"}}, text) && text.find("again") == 0 && state.starts == 2,
          "mcp: the server ended by the time limit starts again at the next call");
    std::atomic<bool> stop{false};
    std::thread stopper([&] {
        std::this_thread::sleep_for(50ms);
        stop = true;
    });
    check(!hub.call("srv", "slow", json::object(), text, [&] { return stop.load(); }) && text == "stopped",
          "mcp: a stop ends a call");
    stopper.join();
    const json d = hub.describe();
    check(d.size() == 2 && d[0].value("state", "") == "running" && d[0]["tools"][0].value("name", "") == "srv_echo" &&
              d[1].value("state", "") == "failed" &&
              d[1].value("error", "").find("no such program") != std::string::npos,
          "mcp: describe lists each server, its state and tools, and why one failed");
    check(!hub.call("broken", "x", json::object(), text) && text.find("not running") != std::string::npos,
          "mcp: a server that could not start refuses calls");
    hub.shutdown();
    check(hub.tools().empty() && !hub.call("srv", "echo", json::object(), text), "mcp: after shutdown nothing starts");
}

static void mcp_agent_tests() {
    FakeMcp state;
    auto hub = std::make_unique<McpHub>(mcp_servers({"srv", "read"}), fake_mcp_start(state));
    McpHub * h = hub.get();
    auto rig = std::make_unique<Rig>(true, true, [h](AgentDeps & d) { d.mcp = h; });
    {
        const std::string c = rig->conversation("ask");
        rig->say({{"", {call("srv_echo", {{"text", "hi"}})}}, {"It said hi.", {}}});
        json item;
        rig->agent->send(c, "echo", false, false, item);
        check(rig->wait_state(c, "waiting"), "mcp ask: a call waits for an approval, like a change");
        const GenerateRequest r = rig->last_request();
        check(declares(r, "srv_echo") && declares(r, "read_file"),
              "mcp: its tools are offered beside the built-in ones");
        size_t read_file_count = 0;
        for (const ChatTool & t : r.tools)
            read_file_count += t.name == "read_file";
        check(read_file_count == 1, "mcp: a tool named like a built-in one is left out");
        check(rig->agent->approve(c, rig->waiting_item(c), true).ok() && rig->wait_done(c),
              "mcp ask: approved, it runs");
        const json tool = rig->items_of(c, "tool")[0];
        check(tool.value("state", "") == "done" && tool.value("result", "").find("hi") == 0 &&
                  tool.value("mcp", "") == "srv",
              "mcp: the result reaches the item, which names its server");
    }
    {
        const std::string c = rig->conversation("full");
        rig->say({{"", {call("srv_echo", {{"text", "yo"}})}}, {"ok", {}}});
        json item;
        rig->agent->send(c, "echo", false, false, item);
        check(rig->wait_done(c) && rig->items_of(c, "tool")[0].value("state", "") == "done",
              "mcp full: a call runs without asking");
    }
    {
        const std::string c = rig->conversation("read_only");
        rig->say({{"", {call("srv_echo", {{"text", "no"}})}}, {"ok", {}}});
        json item;
        rig->agent->send(c, "echo", false, false, item);
        check(rig->wait_done(c) && !declares(rig->last_request(), "srv_echo") &&
                  rig->items_of(c, "tool")[0].value("state", "") == "error",
              "mcp read_only: not offered, and a call is refused");
    }
    {
        const std::string c = rig->conversation("full", false);
        rig->say({{"ok", {}}});
        json item;
        rig->agent->send(c, "hi", false, false, item);
        check(rig->wait_done(c) && !declares(rig->last_request(), "srv_echo"), "mcp: no workspace, no MCP tools");
    }
    {
        const std::string c = rig->conversation("full");
        rig->say({{"ok", {}}});
        json item;
        rig->agent->send(c, "hi", false, true, item);
        check(rig->wait_done(c) && !declares(rig->last_request(), "srv_echo"),
              "mcp: a request from the network gets no MCP tool");
    }
    const json caps = rig->agent->capabilities(true);
    check(caps["mcp"].size() == 2 && caps["mcp"][0].value("name", "") == "srv" &&
              rig->agent->capabilities(false)["mcp"].empty(),
          "mcp: GET /api/agent lists the servers and their tools to a local client only");
    rig.reset();
    hub.reset();
}

static void prompt_tests() {
    // The real bundled skills, as a release ships them beside the server.
    const fs::path skills = fs::path(__FILE__).parent_path().parent_path() / "skills";
    auto rig = std::make_unique<Rig>(true, true, [&](AgentDeps & d) {
        d.bundled_skills_dir = skills.u8string();
        d.os_name = "macOS 26.0";
    });
    const std::string c = rig->conversation("ask");
    rig->say({{"ok", {}}});
    json item;
    rig->agent->send(c, "hi", false, false, item);
    rig->wait_done(c);
    const std::string prompt = rig->last_request().messages[0].content;
    std::printf("system prompt of a workspace conversation: %zu characters, %zu tools\n", prompt.size(),
                rig->last_request().tools.size());
    check(prompt.size() < 4000, "the system prompt (tools apart) stays under 4,000 characters (~1,000 tokens)");
    check(prompt.find("AGENTS.md") == std::string::npos, "no AGENTS.md, none mentioned");

    spit(rig->ws.path / "AGENTS.md", "Use tabs.\n" + std::string(20000, 'x'));
    const std::string c2 = rig->conversation("ask");
    rig->say({{"ok", {}}});
    rig->agent->send(c2, "hi", false, false, item);
    rig->wait_done(c2);
    const std::string with = rig->last_request().messages[0].content;
    check(with.find("AGENTS.md:\nUse tabs.") != std::string::npos &&
              with.size() < prompt.size() + k_agents_md_max_bytes + 200,
          "the workspace's AGENTS.md is appended, capped");
    const std::string c3 = rig->conversation("ask");
    rig->say({{"ok", {}}});
    rig->agent->send(c3, "hi", false, true, item);
    rig->wait_done(c3);
    check(rig->last_request().messages[0].content.find("Use tabs.") == std::string::npos,
          "a request from the network does not get the workspace's AGENTS.md");
}

int main() {
    confinement_tests();
    classifier_tests();
    diff_tests();
    workspace_tool_tests();
    mcp_tests();
    skill_tests();
    schedule_tests();
    provider_tests();
    connect_tests();
    store_tests();
    access_tests();
    audience_tests();
    external_agent_tests();
    command_tests();
    loop_tests();
    prompt_tests();
    mcp_agent_tests();
    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall agent checks passed\n");
    return 0;
}
