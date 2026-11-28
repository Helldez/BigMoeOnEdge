// bmoe-server end to end over HTTP, with the engine and the planner replaced by scripted ones
// (server_seams.h): the server's own policy on a real listening socket, no model. Started without
// --autoload, the way the desktop and the apps run it.
//
//   * a model named at launch is planned and loaded on the plan as made, the side it names included:
//     one open, no model opened to try a side, none reopened before or after the first answer;
//   * a load whose named side will not open opens the plan's other side instead, and the failure is
//     kept against the side, one more each time: a second failure reaches the count a plan drops the
//     side at (MeasuredKnobs::unviable_max);
//   * where the other side does not open either, the load ends in error and nothing is kept.
//   * cancel and unload during auto planning discard even a successful late measurement, without
//     opening a session, changing settings or records, or leaving waiting turns asleep;
//   * standalone measurements have fresh cancellation jobs, preserve the cached plan when stopped,
//     and end their planning state on both cancellation and success.
//
// Every wait is for an event (a state, a count), never for time to pass.
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "listen_port.h"
#include "server.h"
#include "server_seams.h"

#include <cpp-httplib/httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define BMOE_PID _getpid()
#else
#include <unistd.h>
#define BMOE_PID getpid()
#endif

using namespace bmoe;
using namespace bmoe::server;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

static int failures = 0;

// The most any one event here may take: a ceiling, never a pace. Every wait is for an event (the
// port bound, a state, a count), which on a fast machine comes in milliseconds; the ceiling is for the
// slowest runner, where loading the server's own libraries alone has taken tens of seconds, and only
// a server that never answers reaches it.
constexpr std::chrono::seconds k_ceiling{180};

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++failures;
}

namespace {

// A session that answers "hi" to every turn.
class EchoSession final : public HostedSession {
public:
    RunResult generate(const GenerateRequest &, const std::function<void(const TokenMetrics &)> & on_token) override {
        TokenMetrics m;
        m.step = 1;
        m.steps = 1;
        m.piece = "hi";
        m.text = "hi";
        if (on_token) on_token(m);
        RunResult r;
        r.ok = true;
        r.generated_text = "hi";
        r.summary.n_generated = 1;
        r.summary.s_per_token = 0.01;
        return r;
    }
    DecideResult decide(const DecideRequest &) override { return {}; }
    void cancel() override {}
    const std::string & arch() const override { return arch_; }
    int n_ctx() const override { return 32768; }
    int n_expert_used() const override { return 8; }
    ThinkControl think_control() const override { return ThinkControl::Template; }
    double load_seconds() const override { return 0.01; }

private:
    std::string arch_ = "fakemoe";
};

// What the scripted engine and planner did, and what the test tells them to do next.
struct Script {
    std::mutex m;
    std::condition_variable cv;
    int opened = 0;        // sessions opened
    int open_failures = 0; // opens made to fail
    int fail_next_opens = 0;
    int plans = 0; // plans made
    int held_plan = 0;
    int released_plan = 0;
    int plan_gate_timeouts = 0;

    int hold_next_plan() {
        std::lock_guard<std::mutex> lk(m);
        return held_plan = plans + 1;
    }
    void release_plan(int plan) {
        std::lock_guard<std::mutex> lk(m);
        released_plan = plan;
        changed();
    }

    template <class F> bool wait(F pred, std::chrono::milliseconds limit = k_ceiling) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, limit, pred);
    }
    void changed() { cv.notify_all(); }
};

json get_json(httplib::Client & cli, const char * path) {
    auto res = cli.Get(path);
    if (!res || res->status != 200) return json::object();
    try {
        return json::parse(res->body);
    } catch (...) {
        return json::object();
    }
}

int post(httplib::Client & cli, const char * path, const json & body, const char * method = "POST") {
    auto res = std::string(method) == "PUT" ? cli.Put(path, body.dump(), "application/json")
                                            : cli.Post(path, body.dump(), "application/json");
    return res ? res->status : -1;
}

int chat(httplib::Client & cli) {
    return post(cli, "/v1/chat/completions",
                {{"messages", json::array({{{"role", "user"}, {"content", "hello"}}})}, {"max_tokens", 4}});
}

struct HttpReply {
    int status = -1;
    json body = json::object();
};

// Concurrent requests must not share httplib's client or its connection.
HttpReply request(int port, const char * method, const char * path, const json & body = json::object()) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(k_ceiling);
    cli.set_read_timeout(k_ceiling);
    cli.set_write_timeout(k_ceiling);
    auto res = std::string(method) == "GET" ? cli.Get(path) : cli.Post(path, body.dump(), "application/json");
    if (!res) return {};
    json parsed = json::parse(res->body, nullptr, false);
    return {res->status, parsed.is_discarded() ? json::object() : parsed};
}

HttpReply finish(std::future<HttpReply> & pending, const std::string & what) {
    const bool done = pending.wait_for(k_ceiling) == std::future_status::ready;
    check(done, what + " returns");
    return done ? pending.get() : HttpReply{};
}

// Two turns admitted while planning leave one holding the serving ticket and one in the line.
// The metric proves they reached the host before we cancel; a client-side "started" flag would not.
bool wait_deferred(httplib::Client & cli) {
    const auto end = std::chrono::steady_clock::now() + k_ceiling;
    while (std::chrono::steady_clock::now() < end) {
        auto res = cli.Get("/metrics");
        if (res && res->status == 200 && res->body.find("llamacpp:requests_deferred 1\n") != std::string::npos)
            return true;
    }
    return false;
}

// Wait until /api/info satisfies `pred`; it changes on the server's own threads.
bool wait_info(httplib::Client & cli, const std::function<bool(const json &)> & pred) {
    const auto end = std::chrono::steady_clock::now() + k_ceiling;
    while (std::chrono::steady_clock::now() < end) {
        if (pred(get_json(cli, "/api/info"))) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

bool ready(const json & i) {
    return i.value("state", "") == "ready";
}

bool empty_host(const json & i) {
    return i.value("state", "") == "empty" && i.contains("model") && i["model"].is_null() &&
           i.value("error", "missing") == "" && !i.value("generating", true);
}

std::string config_value(httplib::Client & cli, const char * key) {
    const json c = get_json(cli, "/api/config");
    if (!c.contains("values") || !c["values"].contains(key)) return "";
    const json & v = c["values"][key];
    return v.is_string() ? v.get<std::string>() : v.dump();
}

std::string read_knobs(const fs::path & routing) {
    std::error_code ec;
    for (const auto & e : fs::directory_iterator(routing, ec))
        if (e.path().extension() == ".knobs") {
            std::ifstream f(e.path());
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
    return "";
}

std::string read_file(const fs::path & path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

json read_records(const fs::path & routing) {
    json records = json::object();
    std::error_code ec;
    for (const auto & e : fs::directory_iterator(routing, ec))
        if (e.is_regular_file()) records[e.path().filename().u8string()] = read_file(e.path());
    return records;
}

} // namespace

int main() {
    static int counter = 0;
    const fs::path dir = fs::temp_directory_path() / ("bmoe-server-app-test-" + std::to_string((long long) BMOE_PID) +
                                                      "-" + std::to_string(++counter));
    fs::remove_all(dir);
    fs::create_directories(dir / "models");
    const fs::path model = dir / "models" / "fake.gguf";
    {
        std::ofstream f(model, std::ios::binary);
        f << "not a real model " << model.filename().u8string();
    }

    Script sc;
    ServerSeams seams;
    seams.open_session = [&](const SessionConfig &, std::string & error) -> std::unique_ptr<HostedSession> {
        std::lock_guard<std::mutex> lk(sc.m);
        if (sc.fail_next_opens > 0) {
            --sc.fail_next_opens;
            ++sc.open_failures;
            error = "out of device memory";
            sc.changed();
            return nullptr;
        }
        ++sc.opened;
        sc.changed();
        return std::make_unique<EchoSession>();
    };
    // A plan that names a side of the device tier, as one beside a card of its own does; its own value
    // `trial-device-cache` tells the plan as made from the other side's.
    seams.plan = [&](const RunConfig & cfg) {
        std::unique_lock<std::mutex> lk(sc.m);
        const int serial = ++sc.plans;
        const bool held = serial == sc.held_plan;
        sc.changed();
        // Deliberately ignore cancellation: acceptance must reject even a successful result from
        // a probe that could not stop inside its current stage. Time only bounds a broken test.
        if (held && !sc.cv.wait_for(lk, k_ceiling, [&] { return sc.released_plan >= serial; })) ++sc.plan_gate_timeouts;
        lk.unlock();
        PlanOutcome p;
        p.ok = true;
        p.hardware = "0123456789abcdef";
        p.body = json{{"available", true}, {"model", cfg.model_path}, {"script_plan", serial}};
        p.values = {{"trial-device-cache", "1"}};
        if (held) {
            p.values["ctx-size"] = "16384";
            // An accepted auto plan would arm an unconfirmed verdict before the opener runs.
            p.values["prefill-device"] = "script-device";
            p.decisions = json::array({{{"key", "ctx-size"}, {"reason", "a held scripted measurement"}}});
        }
        auto basis = std::make_shared<PlanBasis>();
        basis->cfg = cfg;
        basis->facts.plan.config = cfg;
        basis->facts.plan.config.trial.device_tier = 1;
        p.basis = basis;
        return p;
    };
    seams.load_backends = false; // nothing here computes; the start does not wait on the machine's driver
    std::promise<int> port_promise;
    seams.bound = [&](int port) { port_promise.set_value(port); };

    Embedding emb;
    emb.seams = &seams;
    std::promise<std::function<void()>> stop_promise;
    std::atomic<bool> stop_set{false};
    emb.listening = [&](std::function<void()> stop) {
        if (stop && !stop_set.exchange(true)) stop_promise.set_value(stop);
    };

    // Another program's server holds the port this one prefers, with the library's default options (a
    // port shared where the stack allows it): the server must move past it, not share it, and say
    // where it went in the port file.
    httplib::Server holder;
    holder.Get("/",
               [](const httplib::Request &, httplib::Response & res) { res.set_content("not bmoe", "text/plain"); });
    // A port with room for the fallback after it: the system may hand out one at the top of the range.
    int held = holder.bind_to_any_port("127.0.0.1");
    for (int i = 0; i < 16 && held > kPortMax - kPortFallbackTries; ++i) {
        holder.stop();
        held = holder.bind_to_any_port("127.0.0.1");
    }
    std::thread holding([&] { holder.listen_after_bind(); });
    holder.wait_until_ready();
    check(held > 0, "another server holds a port");

    const std::string dir_s = dir.u8string(), models_s = (dir / "models").u8string(), model_s = model.u8string();
    const std::string port_file = (dir / "server.port").u8string();
    std::vector<std::string> args = {"bmoe-server",     "--host",      "127.0.0.1", "--port",     std::to_string(held),
                                     "--port-fallback", "--port-file", port_file,   "--data-dir", dir_s,
                                     "--models-dir",    models_s,      "--model",   model_s};
    std::vector<char *> argv;
    for (std::string & a : args)
        argv.push_back(a.data());
    std::thread server([&] { run((int) argv.size(), argv.data(), emb); });

    // The port is reported as soon as the socket is bound, before the server's start-up work; a request
    // sent before the server serves waits in the listen queue.
    auto port_future = port_promise.get_future();
    auto stop_future = stop_promise.get_future();
    const bool bound = port_future.wait_for(k_ceiling) == std::future_status::ready;
    check(bound, "the server binds a port");
    if (!bound) {
        server.detach();
        holder.stop();
        holding.join();
        return 1;
    }
    const int port = port_future.get();
    check(port != held && port > held && port < held + kPortFallbackTries,
          "a port in use is moved past, to a free one after it");
    check(read_port_file(port_file) == port, "the port taken is in the port file once bound");
    holder.stop();
    holding.join();
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(k_ceiling);
    cli.set_read_timeout(k_ceiling);
    cli.set_write_timeout(k_ceiling);

    const auto opened = [&] {
        std::lock_guard<std::mutex> lk(sc.m);
        return sc.opened;
    };
    const auto plans = [&] {
        std::lock_guard<std::mutex> lk(sc.m);
        return sc.plans;
    };
    const auto knobs = [&] { return read_knobs(dir / "routing"); };
    const auto open_attempts = [&] {
        std::lock_guard<std::mutex> lk(sc.m);
        return sc.opened + sc.open_failures;
    };
    // Host state changes on unload; only these settings and on-disk effects must stay unchanged.
    const auto effects = [&] {
        const json c = get_json(cli, "/api/config");
        return json{{"values", c.value("values", json::object())},
                    {"plan", c.value("plan", json())},
                    {"settings", read_file(dir / "settings.json")},
                    {"records", read_records(dir / "routing")}};
    };

    // ── the model named at launch: planned once, and loaded on the plan as made ──
    check(wait_info(cli, ready) && opened() == 1 && plans() == 1,
          "the model named at launch is planned once and opened once");
    check(config_value(cli, "trial-device-cache") == "1", "it runs the plan as made, the side it names included");
    check(chat(cli) == 200 && chat(cli) == 200 && opened() == 1 && plans() == 1 &&
              get_json(cli, "/api/info").value("state", "") == "ready",
          "the first answers come from that load: no model is opened to try a side, none reopened");
    check(knobs().find("unviable") == std::string::npos, "a side that opened has no failure kept");

    // A load of the model whose first `fail` opens fail.
    const auto load_failing = [&](int fail) {
        std::lock_guard<std::mutex> lk(sc.m);
        sc.fail_next_opens = fail;
    };

    // ── a load whose named side will not open opens the other side, and remembers ──
    {
        load_failing(1);
        const int failed = sc.open_failures, before = opened();
        check(post(cli, "/api/session/load", {{"model", model.u8string()}}) == 202, "a load whose side will fail");
        check(sc.wait([&] { return sc.open_failures > failed && sc.opened > before; }) && wait_info(cli, ready) &&
                  opened() == before + 1,
              "the named side fails to open, and the other side is loaded instead");
        check(config_value(cli, "trial-device-cache") != "1", "the plan served is the other side's");
        check(knobs().find("unviable=device_cache:1") != std::string::npos, "the failure is kept against the side");
        check(chat(cli) == 200, "and the other side answers");
    }

    // ── neither side opens: an error, and nothing more is kept against the side ──
    {
        load_failing(2);
        const int failed = sc.open_failures, before = opened();
        check(post(cli, "/api/session/load", {{"model", model.u8string()}}) == 202, "a load neither side of opens");
        check(sc.wait([&] {
            return sc.open_failures >= failed + 2;
        }) && wait_info(cli, [](const json & i) { return i.value("state", "") == "error" && i["model"].is_null(); }),
              "it ends in error with no model named");
        check(opened() == before && knobs().find("unviable=device_cache:1") != std::string::npos,
              "nothing opened, and the count is as it was: only an open of the other side says the failure was the "
              "side's");
    }

    // ── a second failure reaches the count a plan drops the side at ──
    {
        load_failing(1);
        const int failed = sc.open_failures, before = opened();
        check(post(cli, "/api/session/load", {{"model", model.u8string()}}) == 202, "the side fails a second time");
        check(sc.wait([&] { return sc.open_failures > failed && sc.opened > before; }) && wait_info(cli, ready),
              "the other side is loaded again");
        check(knobs().find("unviable=device_cache:" + std::to_string(MeasuredKnobs::unviable_max)) != std::string::npos,
              "the second failure is counted on the first: the side has reached the limit");
    }

    // A standalone measurement after a stop must have its own fresh job. It caches, but does not
    // apply, its result, and its RAII end must leave no model "coming" for the next request.
    const auto measure_fresh = [&](const std::string & why) {
        const int before = plans(), before_opens = open_attempts();
        const json before_effects = effects();
        const HttpReply measured = request(port, "GET", "/api/plan?measure=1");
        check(measured.status == 200 && measured.body.value("available", false) &&
                  !measured.body.value("cancelled", false) && measured.body.value("script_plan", 0) == before + 1,
              why + ": a fresh standalone measurement succeeds");
        const json cached = get_json(cli, "/api/plan?cached=1");
        check(cached.value("cached", false) && cached.value("script_plan", 0) == before + 1 && plans() == before + 1,
              why + ": exactly that measurement replaces the cached plan");
        check(empty_host(get_json(cli, "/api/info")) && effects() == before_effects && open_attempts() == before_opens,
              why + ": measurement ends planning without applying settings, writing records or opening a model");
    };
    const json chat_body = {{"messages", json::array({{{"role", "user"}, {"content", "hello"}}})}, {"max_tokens", 4}};
    const auto ask_chat = [&] { return request(port, "POST", "/v1/chat/completions", chat_body); };

    // ── #746: an auto load stopped inside measurement cannot load later ──
    for (const char * stop : {"/api/cancel", "/api/session/unload"}) {
        const std::string why = std::string("auto planning, ") + stop;
        const int before = plans(), before_opens = open_attempts();
        const json before_effects = effects();
        const int held_plan = sc.hold_next_plan();
        // The model is already selected. No request-side settings write can mask a plan's effects.
        auto loading = std::async(std::launch::async, [&] { return request(port, "POST", "/api/session/load"); });
        check(sc.wait([&] { return sc.plans == held_plan; }) &&
                  get_json(cli, "/api/info").value("state", "") == "planning" && open_attempts() == before_opens,
              why + ": the auto load is held in planning, before any opener");
        const json cached_before = get_json(cli, "/api/plan?cached=1");
        auto first_turn = std::async(std::launch::async, ask_chat);
        auto second_turn = std::async(std::launch::async, ask_chat);
        check(wait_deferred(cli), why + ": chat turns are admitted and waiting for the planned model");
        check(request(port, "POST", stop).status == 200, why + ": the stop answers while the probe is still held");
        sc.release_plan(held_plan);
        const HttpReply loaded = finish(loading, why + ": load");
        check(loaded.status == 400 && loaded.body.value("error", "") == "the load was stopped",
              why + ": a late successful plan is refused as a stopped load");
        const HttpReply first = finish(first_turn, why + ": first waiting turn");
        const HttpReply second = finish(second_turn, why + ": second waiting turn");
        check(first.status == 503 && first.body.value("code", "") == "no_model" && second.status == 503 &&
                  second.body.value("code", "") == "no_model",
              why + ": waiting turns wake with no model, rather than answering from a delayed load");
        check(empty_host(get_json(cli, "/api/info")) && open_attempts() == before_opens && plans() == before + 1,
              why + ": the host stays empty, with one measurement and no delayed or fallback opener");
        check(effects() == before_effects, why + ": no plan settings, persisted settings or knob records changed");
        check(get_json(cli, "/api/plan?cached=1") == cached_before, why + ": the rejected plan was not cached");
        measure_fresh(why);
    }

    // ── #746: stopping a standalone measurement preserves last_plan, not a sticky stop flag ──
    for (const char * stop : {"/api/cancel", "/api/session/unload"}) {
        const std::string why = std::string("standalone planning, ") + stop;
        const int before = plans(), before_opens = open_attempts();
        const json before_effects = effects(), cached_before = get_json(cli, "/api/plan?cached=1");
        const int held_plan = sc.hold_next_plan();
        auto measuring = std::async(std::launch::async, [&] { return request(port, "GET", "/api/plan?measure=1"); });
        check(sc.wait([&] { return sc.plans == held_plan; }) &&
                  get_json(cli, "/api/info").value("state", "") == "planning",
              why + ": the standalone measurement owns an admitted planning job");
        check(request(port, "POST", stop).status == 200, why + ": the stop answers before the probe returns");
        // A refused concurrent request must neither measure nor re-admit/reset the active job,
        // including after unload has already cleared the visible planning state.
        check(request(port, "GET", "/api/plan?measure=1").status == 409 && plans() == before + 1,
              why + ": an overlapping measurement is refused without resetting the stopped job");
        sc.release_plan(held_plan);
        const HttpReply measured = finish(measuring, why + ": measurement");
        check(measured.status == 200 && measured.body.value("available", false) &&
                  measured.body.value("cancelled", false) && measured.body.value("declined", false) &&
                  measured.body.value("decline_reason", "") == "the measurement was stopped" &&
                  !measured.body.contains("script_plan"),
              why + ": a late success is returned as an available, cancelled, declined measurement");
        check(get_json(cli, "/api/plan?cached=1") == cached_before,
              why + ": cancellation does not replace the last successful cached plan");
        check(empty_host(get_json(cli, "/api/info")) && open_attempts() == before_opens && plans() == before + 1 &&
                  effects() == before_effects,
              why + ": planning ends empty with exact serial counts and no settings, records or opener effects");
        measure_fresh(why);
    }
    {
        std::lock_guard<std::mutex> lk(sc.m);
        check(sc.plan_gate_timeouts == 0, "every held measurement was released by an event, never by its watchdog");
    }

    const bool listening = stop_future.wait_for(k_ceiling) == std::future_status::ready;
    check(listening, "the server is listening");
    if (!listening) {
        server.detach();
        return 1;
    }
    stop_future.get()();
    server.join();
    std::error_code ec;
    fs::remove_all(dir, ec);

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall server app checks passed\n");
    return 0;
}
