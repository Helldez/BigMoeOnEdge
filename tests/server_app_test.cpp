// bmoe-server end to end over HTTP, with the engine and the planner replaced by scripted ones
// (server_seams.h): the server's own policy on a real listening socket, no model. Started without
// --autoload, the way the desktop and the apps run it.
//
//   * a model named at launch is planned and loaded, and answers;
//   * a plan that leaves a decision across loads pending is calibrated once the server is idle after
//     an answer: /api/info reads ready with `calibrating`;
//   * a request during the calibration is answered, not refused: it stops the calibration, which
//     applies what it decided and loads again;
//   * one attempt per load: the next answer's end finds nothing armed;
//   * a cancel stops it and the model loads again; an unload stops it and nothing is opened or
//     applied; another model chosen meanwhile is not given this model's plan; a load asked for
//     meanwhile supersedes it; a final open that fails leaves no model named;
//   * a served load whose pending side will not open opens the plan's other side instead, and the
//     failure is kept against the side; a side another load already measured is served instead of a
//     pending one that has not been proven to open.
//
// Every wait is for an event (a state, a count, a turn's end), never for time to pass.
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

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
    int calibrations = 0;
    bool calibrating = false;
    bool stopped = false; // the last calibration saw its stop
    bool release = false; // ends the calibration in progress without a stop
    int turn_ends = 0;
    bool last_turn_armed = false;
    // What the next plan's record holds: the pending side's other side measured or not.
    bool other_measured = false;

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
    return i.value("state", "") == "ready" && !i.value("calibrating", true);
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

} // namespace

int main() {
    static int counter = 0;
    const fs::path dir = fs::temp_directory_path() / ("bmoe-server-app-test-" + std::to_string((long long) BMOE_PID) +
                                                      "-" + std::to_string(++counter));
    fs::remove_all(dir);
    fs::create_directories(dir / "models");
    const fs::path model = dir / "models" / "fake.gguf", other = dir / "models" / "other.gguf";
    for (const fs::path & p : {model, other}) {
        std::ofstream f(p, std::ios::binary);
        f << "not a real model " << p.filename().u8string();
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
    // A plan that leaves the device tier to time, as one beside a card of its own does; its own value
    // `trial-device-cache` tells the served plan from the other side's.
    seams.plan = [&](const RunConfig & cfg) {
        PlanOutcome p;
        p.ok = true;
        p.hardware = "0123456789abcdef";
        p.body = json{{"available", true}, {"model", cfg.model_path}};
        p.values = {{"trial-device-cache", "1"}};
        auto basis = std::make_shared<PlanBasis>();
        basis->cfg = cfg;
        basis->facts.plan.config = cfg;
        basis->facts.plan.config.trial.device_tier = 1;
        {
            std::lock_guard<std::mutex> lk(sc.m);
            if (sc.other_measured) basis->facts.model.knobs.no_device_cache_tps = 9.0;
        }
        p.basis = basis;
        return p;
    };
    // A calibration that runs until a request stops it or the test releases it, and then decides a
    // plan that sets `threads` to 5.
    seams.calibrate = [&](const PlanOutcome & p, const MeasuredKnobs &, const std::atomic<bool> & stop,
                          const PlanStepFn & on_step) {
        {
            std::lock_guard<std::mutex> lk(sc.m);
            ++sc.calibrations;
            sc.calibrating = true;
            sc.release = false;
            sc.changed();
        }
        if (on_step) on_step("calibrate", "Timing the placements", 0, 4);
        for (;;) {
            std::unique_lock<std::mutex> lk(sc.m);
            if (stop.load() || sc.release) break;
            sc.cv.wait_for(lk, 1ms);
        }
        CalibrationOutcome c;
        c.ran = true;
        c.complete = true;
        c.plan = p;
        c.plan.values = {{"threads", "5"}};
        auto decided = std::make_shared<PlanBasis>(*p.basis);
        decided->facts.plan.config.trial.device_tier = -1;
        c.plan.basis = decided;
        std::lock_guard<std::mutex> lk(sc.m);
        sc.stopped = stop.load();
        sc.calibrating = false;
        sc.changed();
        return c;
    };
    seams.calibration_idle_delay = 20ms;
    seams.load_backends = false; // nothing here computes; the start does not wait on the machine's driver
    std::promise<int> port_promise;
    seams.bound = [&](int port) { port_promise.set_value(port); };
    seams.turn_ended = [&](bool armed) {
        std::lock_guard<std::mutex> lk(sc.m);
        ++sc.turn_ends;
        sc.last_turn_armed = armed;
        sc.changed();
    };

    Embedding emb;
    emb.seams = &seams;
    std::promise<std::function<void()>> stop_promise;
    std::atomic<bool> stop_set{false};
    emb.listening = [&](std::function<void()> stop) {
        if (stop && !stop_set.exchange(true)) stop_promise.set_value(stop);
    };

    const std::string dir_s = dir.u8string(), models_s = (dir / "models").u8string(), model_s = model.u8string();
    std::vector<std::string> args = {"bmoe-server", "--host",       "127.0.0.1", "--port",  "0",    "--data-dir",
                                     dir_s,         "--models-dir", models_s,    "--model", model_s};
    std::vector<char *> argv;
    for (std::string & a : args)
        argv.push_back(a.data());
    std::thread server([&] { run((int) argv.size(), argv.data(), emb); });

    // The port is reported as soon as the socket is bound, before the server's start-up work; a request
    // sent before the server serves waits in the listen queue.
    auto port_future = port_promise.get_future();
    auto stop_future = stop_promise.get_future();
    const bool bound = port_future.wait_for(k_ceiling) == std::future_status::ready;
    check(bound, "the server binds the port the system chose");
    if (!bound) {
        server.detach();
        return 1;
    }
    httplib::Client cli("127.0.0.1", port_future.get());
    cli.set_connection_timeout(k_ceiling);
    cli.set_read_timeout(k_ceiling);
    cli.set_write_timeout(k_ceiling);

    const auto opened = [&] {
        std::lock_guard<std::mutex> lk(sc.m);
        return sc.opened;
    };
    const auto calibrations = [&] {
        std::lock_guard<std::mutex> lk(sc.m);
        return sc.calibrations;
    };
    // An answer, then the calibration it arms, running.
    const auto answer_then_calibrating = [&](const std::string & what) {
        const int before = calibrations();
        check(chat(cli) == 200, what + ": an answer");
        check(sc.wait([&] { return sc.calibrations > before && sc.calibrating; }),
              what + ": idle after it, the pending side is calibrated");
    };
    const auto load = [&](const fs::path & m) {
        const int before = opened();
        check(post(cli, "/api/session/load", {{"model", m.u8string()}}) == 202 &&
                  sc.wait([&] { return sc.opened > before; }) && wait_info(cli, ready),
              "a load of " + m.filename().u8string());
    };

    // ── the model named at launch, an answer, the calibration, a request during it ──
    check(wait_info(cli, ready) && opened() == 1, "the model named at launch is planned and loaded");
    answer_then_calibrating("first load");
    const json during = get_json(cli, "/api/info");
    check(during.value("state", "") == "ready" && during.value("calibrating", false) && during["model"].is_object(),
          "during the calibration /api/info reads ready, calibrating, with its model");
    check(chat(cli) == 200, "a request during the calibration is answered, not refused");
    check(sc.wait([&] { return !sc.calibrating; }) && sc.stopped, "and it stopped the calibration");
    check(wait_info(cli, ready) && opened() == 2, "the model is loaded again after it");
    check(config_value(cli, "threads") == "5", "the decided plan is applied");
    {
        int ends = 0;
        {
            std::lock_guard<std::mutex> lk(sc.m);
            ends = sc.turn_ends;
        }
        check(chat(cli) == 200 && sc.wait([&] { return sc.turn_ends > ends; }) && !sc.last_turn_armed &&
                  calibrations() == 1,
              "one calibration per load: the next answer's end finds nothing armed");
    }

    // ── a cancel stops it, and the model is loaded again ──
    load(model);
    answer_then_calibrating("cancel");
    {
        const int before = opened();
        check(post(cli, "/api/cancel", json::object()) == 200, "a cancel during the calibration");
        check(sc.wait([&] { return !sc.calibrating; }) && sc.stopped && sc.wait([&] { return sc.opened > before; }) &&
                  wait_info(cli, ready),
              "it stops the calibration, and the model loads again");
    }

    // ── an unload stops it: nothing is opened and nothing applied ──
    load(model);
    answer_then_calibrating("unload");
    {
        const int before = opened();
        check(post(cli, "/api/session/unload", json::object()) == 200, "an unload during the calibration");
        check(sc.wait([&] { return !sc.calibrating; }) && sc.stopped, "it stops the calibration");
        check(get_json(cli, "/api/info").value("state", "") == "empty" && opened() == before &&
                  config_value(cli, "threads") != "5",
              "nothing is opened after it, and its plan is not applied");
    }

    // ── another model chosen meanwhile: this model's plan is not laid over it ──
    load(model);
    answer_then_calibrating("model switch");
    {
        check(post(cli, "/api/config", {{"values", {{"model", other.u8string()}}}}, "PUT") == 200,
              "another model chosen during the calibration");
        {
            std::lock_guard<std::mutex> lk(sc.m);
            sc.release = true;
            sc.changed();
        }
        check(sc.wait([&] { return !sc.calibrating; }) && wait_info(cli, ready), "the calibration ends, a model loads");
        check(config_value(cli, "threads") != "5" && config_value(cli, "model") == other.u8string(),
              "its plan is not applied over the settings that now name another model");
    }

    // ── a load asked for meanwhile supersedes it ──
    load(model);
    answer_then_calibrating("load");
    {
        const int before = opened();
        check(post(cli, "/api/session/load", json::object()) == 202,
              "a load during the calibration is taken, not refused");
        check(sc.wait([&] { return !sc.calibrating && sc.opened > before; }) && wait_info(cli, ready),
              "it supersedes the calibration and loads");
    }

    // ── a final open that fails leaves no model named ──
    load(model);
    answer_then_calibrating("failed open");
    {
        std::lock_guard<std::mutex> lk(sc.m);
        sc.fail_next_opens = 1;
        sc.release = true;
        sc.changed();
    }
    check(wait_info(cli,
                    [](const json & i) {
                        return i.value("state", "") == "error" && i["model"].is_null() && !i.value("calibrating", true);
                    }),
          "a final open that fails ends in error with no model named");

    // ── a served load whose pending side will not open opens the other side, and remembers ──
    {
        {
            std::lock_guard<std::mutex> lk(sc.m);
            sc.fail_next_opens = 1;
        }
        const int failed = sc.open_failures, before = opened();
        check(post(cli, "/api/session/load", {{"model", model.u8string()}}) == 202, "a load whose side will fail");
        check(sc.wait([&] { return sc.open_failures > failed && sc.opened > before; }) && wait_info(cli, ready),
              "the pending side fails to open, and the other side is loaded instead");
        check(config_value(cli, "trial-device-cache") != "1", "the plan served is the other side's");
        check(read_knobs(dir / "routing").find("unviable=device_cache:1") != std::string::npos,
              "the failure is kept against the side");
    }

    // ── a side measured before is served instead of a pending one not proven to open ──
    {
        {
            std::lock_guard<std::mutex> lk(sc.m);
            sc.other_measured = true;
        }
        load(model);
        check(config_value(cli, "trial-device-cache") != "1",
              "with the other side measured here, the pending side is left to the calibration");
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
