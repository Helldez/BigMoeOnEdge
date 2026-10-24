// The engine host's state machine (server/engine_host.h), driven with a scripted session in place
// of a model: what a load, a turn, a cancel and an unload do to one another across threads, and
// which failures close the session. No model, no llama.cpp graph — the session is a fake behind
// the host's one seam (server/hosted_session.h).
//
//   * a load runs in the background and ends ready, or in error with the opener's reason;
//   * a turn with nothing loaded is a 503 `no_model`; a turn sent while a load runs waits for it,
//     then runs on the new session or gets the 503 if the load failed;
//   * cancel() and unload() during a turn stop it: the turn answers cancelled, and unload leaves the
//     host empty with the session gone;
//   * the last full turn is the measured one; a cancelled turn is not, and a load clears it;
//   * a failed decode closes the session (500, state error), while a refused request (400) or
//     rolled-back invalid output (500) keeps it;
//   * decide() follows the same rule on `fatal`;
//   * the engine's diagnostics reach the bus as `log` events while a host lives;
//   * a context short of what a server's clients send is flagged in the ready state and warned of
//     in the log at load, and a context_full refusal is logged with the way to raise it.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "engine_host.h"

#include "bmoe/log.h"
#include "bmoe/params.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace bmoe;
using namespace bmoe::server;
using namespace std::chrono_literals;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout); // a check that hangs the next one is still on record
    if (!ok) ++failures;
}

// A one-shot gate a test opens from its own thread.
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    void release() {
        std::lock_guard<std::mutex> lk(m);
        open = true;
        cv.notify_all();
    }
    bool wait(std::chrono::milliseconds limit = 5s) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, limit, [&] { return open; });
    }
};

// What the fake sessions of one test share with it: how each turn ends, and what happened.
struct Script {
    // How generate() ends. Blocking turns signal `started` and run until cancel(); streaming ones
    // do the same while sending a token every millisecond, counted in `tokens`.
    enum class Turn { Ok, Block, Stream, Fail, ContextFull, InvalidOutput } turn = Turn::Ok;
    DecideResult decide_result;
    int n_ctx = 4096; // the session's context: short of what a server's clients send, as the default is

    Gate started;
    std::atomic<int> opened{0};
    std::atomic<int> alive{0};
    std::atomic<int> turns{0};
    GenerateRequest last_request;
    std::atomic<int> tokens{0};
};

class FakeSession final : public HostedSession {
public:
    explicit FakeSession(Script & s) : s_(s) { ++s_.alive; }
    ~FakeSession() override { --s_.alive; }

    RunResult generate(const GenerateRequest & req,
                       const std::function<void(const TokenMetrics &)> & on_token) override {
        ++s_.turns;
        cancelled_ = false; // as the session does: a cancel latches for the generation it lands in
        s_.last_request = req;
        RunResult r;
        switch (s_.turn) {
        case Script::Turn::Ok: {
            TokenMetrics m;
            m.step = 1;
            m.steps = 1;
            if (on_token) on_token(m);
            r.ok = true;
            r.generated_text = "hi";
            r.summary.n_generated = 1;
            r.summary.s_per_token = 0.1;
            return r;
        }
        case Script::Turn::Block:
            s_.started.release();
            // As the abort callback does: a cancel ends the decode, and the turn answers cancelled.
            for (int i = 0; i < 5000 && !cancelled_.load(); ++i)
                std::this_thread::sleep_for(1ms);
            r.ok = true;
            r.cancelled = cancelled_.load();
            // A turn stopped mid-answer has tokens and a rate too: they must not count as measured.
            r.summary.n_generated = 3;
            r.summary.s_per_token = 0.5;
            return r;
        case Script::Turn::Stream:
            for (int i = 0; i < 5000 && !cancelled_.load(); ++i) {
                TokenMetrics m;
                m.step = i + 1;
                ++s_.tokens;
                if (on_token) on_token(m);
                if (i == 0) s_.started.release();
                std::this_thread::sleep_for(1ms);
            }
            r.ok = true;
            r.cancelled = cancelled_.load();
            return r;
        case Script::Turn::Fail:
            r.error = "decode failed during generation";
            r.error_kind = RunError::Failed;
            return r;
        case Script::Turn::ContextFull:
            r.error = "the conversation exceeds the session n_ctx";
            r.error_kind = RunError::ContextFull;
            return r;
        case Script::Turn::InvalidOutput:
            r.error = "JSON output constraint produced invalid JSON";
            r.error_kind = RunError::InvalidOutput;
            return r;
        }
        return r;
    }
    DecideResult decide(const DecideRequest &) override { return s_.decide_result; }
    void cancel() override { cancelled_ = true; }

    const std::string & arch() const override { return arch_; }
    int n_ctx() const override { return s_.n_ctx; }
    int n_expert_used() const override { return 8; }
    ThinkControl think_control() const override { return ThinkControl::Template; }
    double load_seconds() const override { return 0.25; }

private:
    Script & s_;
    std::atomic<bool> cancelled_{false};
    std::string arch_ = "fakemoe";
};

// An opener over `script`. A non-null `hold` keeps the load open until the test releases it; a
// non-empty `fail` makes the load fail with that reason.
static SessionOpener opener(Script & script, Gate * hold = nullptr, std::string fail = "") {
    return [&script, hold, fail](const SessionConfig &, std::string & error) -> std::unique_ptr<HostedSession> {
        if (hold) hold->wait();
        ++script.opened;
        if (!fail.empty()) {
            error = fail;
            return nullptr;
        }
        return std::make_unique<FakeSession>(script);
    };
}

static RunConfig config() {
    RunConfig c;
    c.model_path = "models/fake.gguf"; // never opened: the fake opener stands in for the load
    return c;
}

static std::string state(const EngineHost & h) {
    return h.info().value("state", "");
}

// Wait until the host's state reads `want`, or give up.
static bool wait_state(const EngineHost & h, const std::string & want, std::chrono::milliseconds limit = 5s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (state(h) == want) return true;
        std::this_thread::sleep_for(2ms);
    }
    return state(h) == want;
}

static ChatRequest turn() {
    ChatRequest r;
    ChatMessage m;
    m.role = "user";
    m.content = "hello";
    r.messages.push_back(m);
    r.n_predict = 4;
    return r;
}

static void load_tests() {
    {
        EventBus bus;
        Script script;
        EngineHost host(bus, opener(script));
        check(state(host) == "empty" && !host.ready(), "a new host holds nothing");
        const ChatOutcome none = host.chat(turn(), nullptr);
        check(none.status == 503 && none.code == error_code::no_model, "a turn with nothing loaded is a 503 no_model");

        check(host.load(config(), config()).empty(), "a valid config starts a load");
        check(wait_state(host, "ready") && host.ready(), "the load ends ready");
        const json info = host.info();
        check(info["model"].value("arch", "") == "fakemoe" && info["model"].value("n_ctx", 0) == 4096 &&
                  info["model"].value("name", "") == "fake.gguf",
              "the ready state describes the session the opener made");
        check(host.model_name() == "fake.gguf" && script.opened == 1, "one load opened one session");
        check(host.reload_keys(config()).empty() && !host.reload_required(config()),
              "the config the session loaded with needs no reload");
        RunConfig changed = config();
        check(apply_flag(changed, "--threads", "3").matched && apply_flag(changed, "--n-predict", "7").matched,
              "the test edits one session row and one request row");
        check(host.reload_keys(changed) == std::vector<std::string>{"threads"} && host.reload_required(changed),
              "only a session row that differs from the loaded session needs a reload");

        RunConfig bad = config();
        bad.model_path.clear();
        check(!host.load(bad, bad).empty() && host.ready(),
              "a config that cannot start is refused, and changes nothing");

        check(host.load(config(), config()).empty() && wait_state(host, "ready"), "a second load replaces the first");
        check(script.opened == 2 && script.alive == 1, "the replaced session is gone before the new one runs");
    }
    {
        EventBus bus;
        Script script;
        EngineHost host(bus, opener(script, nullptr, "failed to load model: models/fake.gguf"));
        host.load(config(), config());
        check(wait_state(host, "error") && !host.ready(), "a failed load ends in error");
        check(host.info().value("error", "") == "failed to load model: models/fake.gguf",
              "the error is the opener's reason");
    }
}

static void loading_tests() {
    {
        EventBus bus;
        Script script;
        Gate hold;
        EngineHost host(bus, opener(script, &hold));
        host.load(config(), config());
        check(wait_state(host, "loading") && host.loaded_or_loading() && !host.ready(), "a held load reads loading");
        check(host.load(config(), config()) == "a model is already loading", "a load while loading is refused");

        // The turn takes its place in line and waits out the load, which holds the generation lock.
        auto pending = std::async(std::launch::async, [&] { return host.chat(turn(), nullptr); });
        check(pending.wait_for(50ms) == std::future_status::timeout, "a turn sent while loading waits for the load");
        hold.release();
        const ChatOutcome out = pending.get();
        check(out.status == 200 && out.result.generated_text == "hi", "then runs on the session the load made");
    }
    {
        EventBus bus;
        Script script;
        Gate hold;
        EngineHost host(bus, opener(script, &hold, "not a MoE this engine streams"));
        host.load(config(), config());
        wait_state(host, "loading");
        auto pending = std::async(std::launch::async, [&] { return host.chat(turn(), nullptr); });
        check(pending.wait_for(50ms) == std::future_status::timeout, "a turn waits for a load that will fail");
        hold.release();
        const ChatOutcome out = pending.get();
        check(out.status == 503 && out.code == error_code::no_model, "and gets the 503 once it has");
    }
}

static void request_tests() {
    EventBus bus;
    Script script;
    EngineHost host(bus, opener(script));
    host.load(config(), config());
    check(wait_state(host, "ready"), "the request-mapping session is ready");
    ChatRequest constrained = turn();
    constrained.think = false;
    constrained.json_schema = R"({"type":"object"})";
    check(host.chat(constrained, nullptr).status == 200 && script.last_request.json_schema == constrained.json_schema &&
              !script.last_request.think && !script.last_request.clear_kv && script.last_request.render_text &&
              script.last_request.messages.size() == 1 && script.last_request.messages.back().content == "hello" &&
              script.last_request.n_predict == 4,
          "a JSON object constraint reaches the session beside the existing request fields");
    check(host.chat(turn(), nullptr).status == 200 && script.last_request.json_schema.empty(),
          "a following ordinary request carries no constraint");
    check(host.chat(constrained, nullptr).status == 200 && script.last_request.json_schema == constrained.json_schema &&
              script.opened == 1,
          "JSON object output can be requested again without reloading the session");
    ChatRequest array = turn();
    array.json_schema = R"({"type":"array","items":{"type":"integer"}})";
    array.think = true;
    array.tools = {{"lookup", "Look up a value", R"({"type":"object"})"}};
    array.tool_choice = ToolChoice::Required;
    check(host.chat(array, nullptr).status == 200 && script.last_request.json_schema == array.json_schema &&
              script.last_request.think && script.last_request.tools.size() == 1 &&
              script.last_request.tools[0].name == "lookup" && script.last_request.tool_choice == ToolChoice::Required,
          "a non-object output schema reaches the session with thinking and tools unchanged");
    check(host.chat(turn(), nullptr).status == 200 && script.last_request.json_schema.empty() &&
              script.last_request.tools.empty() && script.last_request.tool_choice == ToolChoice::Auto,
          "schema, tools and tool choice do not leak into the next ordinary request");
}

static void cancel_tests() {
    {
        EventBus bus;
        Script script;
        script.turn = Script::Turn::Block;
        EngineHost host(bus, opener(script));
        host.load(config(), config());
        wait_state(host, "ready");
        auto pending = std::async(std::launch::async, [&] { return host.chat(turn(), nullptr); });
        check(script.started.wait(), "the turn is generating");
        check(host.generating(), "the host says so");
        host.cancel();
        const ChatOutcome out = pending.get();
        check(out.status == 200 && out.result.cancelled, "cancel() during a turn answers it cancelled");
        check(host.ready() && !host.generating() && script.alive == 1, "and keeps the session");
    }
    {
        EventBus bus;
        Script script;
        script.turn = Script::Turn::Block;
        EngineHost host(bus, opener(script));
        host.load(config(), config());
        wait_state(host, "ready");
        auto pending = std::async(std::launch::async, [&] { return host.chat(turn(), nullptr); });
        script.started.wait();
        host.unload(); // cancels the turn, waits for it, then drops the session
        const ChatOutcome out = pending.get();
        check(out.status == 200 && out.result.cancelled, "unload() during a turn cancels it");
        check(state(host) == "empty" && !host.loaded_or_loading() && script.alive == 0,
              "and leaves the host empty, the session closed");
        const ChatOutcome after = host.chat(turn(), nullptr);
        check(after.status == 503, "the next turn finds no model");
    }
    {
        // A client that goes away mid-answer: its on_token says so, and the host stops its turn.
        EventBus bus;
        Script script;
        script.turn = Script::Turn::Stream;
        EngineHost host(bus, opener(script));
        host.load(config(), config());
        wait_state(host, "ready");
        int seen = 0;
        const ChatOutcome out = host.chat(turn(), [&](const TokenMetrics &) { return ++seen < 3; });
        check(out.status == 200 && out.result.cancelled && seen == 3,
              "a turn whose client is gone (on_token false) is cancelled");
        check(host.ready() && !host.generating(), "and the session stays for the next client");
    }
    {
        // Two clients: one answering, one waiting behind it. The waiting one leaves; the answering
        // one must not notice (#346: a failed keep-alive write used to cancel whatever ran).
        EventBus bus;
        Script script;
        script.turn = Script::Turn::Stream;
        EngineHost host(bus, opener(script));
        host.load(config(), config());
        wait_state(host, "ready");
        auto running = std::async(std::launch::async,
                                  [&] { return host.chat(turn(), [](const TokenMetrics &) { return true; }); });
        script.started.wait();
        std::atomic<bool> asked{false};
        const ChatOutcome left = host.chat(
            turn(), [](const TokenMetrics &) { return true; },
            [&] {
                asked = true;
                return false;
            });
        check(left.status == 499 && asked, "the waiting client that leaves gives up its place");
        const int before = script.tokens.load();
        std::this_thread::sleep_for(30ms);
        check(running.wait_for(0ms) == std::future_status::timeout && script.tokens.load() > before,
              "and the turn that was answering keeps answering");
        host.cancel();
        const ChatOutcome out = running.get();
        check(out.status == 200 && out.result.cancelled, "until its own cancel");
    }
}

// The plan screen shows the loaded model's last turn as "measured": a full turn records it, a
// cancelled one does not, and a load clears it even when a turn of the old model ends after.
static void measured_tests() {
    {
        EventBus bus;
        Script script;
        EngineHost host(bus, opener(script));
        host.load(config(), config());
        wait_state(host, "ready");
        check(host.last_turn().is_null(), "a fresh load has no measured turn");
        host.chat(turn(), nullptr);
        check(host.last_turn().value("s_per_token", 0.0) == 0.1, "a full turn is the measured one");
    }
    {
        EventBus bus;
        Script script;
        script.turn = Script::Turn::Block;
        EngineHost host(bus, opener(script));
        host.load(config(), config());
        wait_state(host, "ready");
        auto pending = std::async(std::launch::async, [&] { return host.chat(turn(), nullptr); });
        script.started.wait();
        host.cancel();
        check(pending.get().result.cancelled && host.last_turn().is_null(), "a cancelled turn is no measurement");
    }
    {
        EventBus bus;
        Script script;
        script.turn = Script::Turn::Ok;
        EngineHost host(bus, opener(script));
        host.load(config(), config());
        wait_state(host, "ready");
        host.chat(turn(), nullptr);
        script.turn = Script::Turn::Block;
        auto pending = std::async(std::launch::async, [&] { return host.chat(turn(), nullptr); });
        script.started.wait();
        check(host.load(config(), config()).empty(), "a load during a turn starts");
        (void) pending.get();
        check(wait_state(host, "ready") && host.last_turn().is_null(),
              "the new model shows no figure measured on the old one");
    }
}

static void failure_tests() {
    check(std::string(run_error_name(RunError::InvalidOutput)) == "invalid_output" &&
              run_error_recoverable(RunError::InvalidOutput) && !run_error_recoverable(RunError::Failed),
          "invalid output has a stable recoverable code, distinct from a failed decode");
    {
        EventBus bus;
        Script script;
        script.turn = Script::Turn::InvalidOutput;
        EngineHost host(bus, opener(script));
        host.load(config(), config());
        check(wait_state(host, "ready"), "the invalid-output session is ready");
        ChatRequest constrained = turn();
        constrained.think = false;
        constrained.json_schema = R"({"type":"object"})";
        const ChatOutcome out = host.chat(constrained, nullptr);
        check(out.status == 500 && out.code == "invalid_output" && !out.result.ok,
              "invalid generated JSON is a server error, not a successful or malformed request");
        check(host.ready() && state(host) == "ready" && script.alive == 1 && script.opened == 1,
              "a rolled-back invalid output keeps the loaded session ready");
        check(host.last_turn().is_null(), "invalid output is not recorded as a completed measurement");
        script.turn = Script::Turn::Ok;
        check(host.chat(turn(), nullptr).status == 200 && script.last_request.json_schema.empty(),
              "an ordinary request succeeds after invalid output");
        check(host.chat(constrained, nullptr).status == 200 &&
                  script.last_request.json_schema == constrained.json_schema && script.alive == 1 && script.opened == 1,
              "a constrained request also succeeds without reloading the session");
    }
    {
        EventBus bus;
        Script script;
        script.turn = Script::Turn::ContextFull;
        EngineHost host(bus, opener(script));
        host.load(config(), config());
        wait_state(host, "ready");
        const ChatOutcome out = host.chat(turn(), nullptr);
        check(out.status == 400 && out.code == "context_full", "a recoverable kind is a 400 with its code");
        check(host.ready() && script.alive == 1, "and keeps the session");
    }
    {
        EventBus bus;
        Script script;
        script.turn = Script::Turn::Fail;
        EngineHost host(bus, opener(script));
        host.load(config(), config());
        wait_state(host, "ready");
        const ChatOutcome out = host.chat(turn(), nullptr);
        check(out.status == 500 && out.code == "failed", "a failed decode is a 500");
        check(state(host) == "error" && script.alive == 0, "and closes the session");
        check(host.info().value("error", "").find("the session failed and was closed") == 0,
              "the error says the session was closed");
        check(host.chat(turn(), nullptr).status == 503, "the next turn finds no model");
    }
    {
        EventBus bus;
        Script script;
        script.decide_result.error = "no choices";
        script.decide_result.error_kind = RunError::InvalidRequest;
        EngineHost host(bus, opener(script));
        host.load(config(), config());
        wait_state(host, "ready");
        const DecideOutcome refused = host.decide(DecideRequest{});
        check(refused.status == 400 && refused.code == "invalid_request" && host.ready(),
              "a refused decision is a 400 and keeps the session");
        script.decide_result.fatal = true;
        script.decide_result.error_kind = RunError::Failed;
        const DecideOutcome failed = host.decide(DecideRequest{});
        check(failed.status == 500 && state(host) == "error" && script.alive == 0,
              "a fatal decision is a 500 and closes the session");
    }
}

static void log_tests() {
    EventBus bus;
    Script script;
    auto q = bus.subscribe();
    {
        EngineHost host(bus, opener(script));
        log_printf(LogLevel::Warn, "prefill stays on the %s", "CPU");
        std::string frame;
        bool seen = false;
        while (EventBus::next(*q, frame, 100ms) && !frame.empty())
            if (frame.find("event: log\n") == 0)
                seen = frame.find("\"level\":\"warn\"") != std::string::npos &&
                       frame.find("\"message\":\"prefill stays on the CPU\"") != std::string::npos;
        check(seen, "an engine diagnostic reaches the bus as a log event");
    }
    log_printf(LogLevel::Info, "after the host");
    std::string frame;
    bool seen = false;
    while (EventBus::next(*q, frame, 50ms) && !frame.empty())
        seen = seen || frame.find("after the host") != std::string::npos;
    check(!seen, "a host gone hands the log back to stderr");
    bus.unsubscribe(q);
}

// The next `log` event on `q` whose message contains `needle`, or "".
static std::string log_with(EventBus::Queue & q, const std::string & needle) {
    std::string frame;
    while (EventBus::next(q, frame, 100ms) && !frame.empty())
        if (frame.find("event: log\n") == 0 && frame.find(needle) != std::string::npos) return frame;
    return "";
}

static void context_tests() {
    const std::string recommended = std::to_string(RunConfig::ctx_for_clients);
    {
        EventBus bus;
        auto q = bus.subscribe();
        Script script; // a 4096-token session, as the server's default makes
        EngineHost host(bus, opener(script));
        host.load(config(), config());
        wait_state(host, "ready");
        const json advice = host.info()["model"].value("ctx_advice", json::object());
        check(advice.value("recommended", 0) == RunConfig::ctx_for_clients && advice.value("short", false) &&
                  find_param(advice.value("param", "")) != nullptr,
              "a short context is flagged in the ready state, with the recommendation and the parameter");
        check(!advice.value("warn", true) && !advice.value("refused", true),
              "but a server only this device reaches does not ask a UI to warn before a refusal");
        const std::string warn = log_with(*q, "under the " + recommended);
        check(warn.find("\"level\":\"warn\"") != std::string::npos &&
                  warn.find(flag_spelling(advice.value("param", ""), recommended)) != std::string::npos,
              "and warned of at load, with the flag that raises it");

        script.turn = Script::Turn::ContextFull;
        check(host.chat(turn(), nullptr).code == "context_full", "a turn past the context is refused");
        const std::string refused = log_with(*q, "refused with context_full");
        check(refused.find("\"level\":\"warn\"") != std::string::npos &&
                  refused.find(flag_spelling(advice.value("param", ""))) != std::string::npos,
              "and the refusal is logged with the way to raise the context");
        const json after = host.info()["model"].value("ctx_advice", json::object());
        check(after.value("refused", false) && after.value("warn", false),
              "and a refusal asks a UI to warn, on a server only this device reaches too");
        bus.unsubscribe(q);
    }
    {
        EventBus bus;
        Script script;
        EngineHost host(bus, opener(script));
        host.set_serves_network(true);
        host.load(config(), config());
        wait_state(host, "ready");
        const json advice = host.info()["model"].value("ctx_advice", json::object());
        check(advice.value("short", false) && advice.value("warn", false),
              "served on the network, a short context asks a UI to warn");
    }
    {
        EventBus bus;
        auto q = bus.subscribe();
        Script script;
        script.n_ctx = RunConfig::ctx_for_clients;
        EngineHost host(bus, opener(script));
        host.set_serves_network(true);
        host.load(config(), config());
        wait_state(host, "ready");
        const json advice = host.info()["model"].value("ctx_advice", json::object());
        check(advice.value("recommended", 0) == RunConfig::ctx_for_clients && !advice.value("short", true) &&
                  !advice.value("warn", true),
              "a context at the recommendation is not short, served on the network or not");
        check(log_with(*q, "under the ").empty(), "and draws no warning");
        bus.unsubscribe(q);
    }
}

int main() {
    load_tests();
    loading_tests();
    request_tests();
    cancel_tests();
    measured_tests();
    failure_tests();
    log_tests();
    context_tests();
    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall engine host checks passed\n");
    return 0;
}
