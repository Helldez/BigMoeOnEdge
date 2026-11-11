// bmoe-server end to end over HTTP, with the engine and the planner replaced by scripted ones
// (server.h, Embedding's test seams): the server's own policy on a real listening socket, no model.
//
//   * a model named at launch is planned and loaded, and answers;
//   * a plan that leaves a decision across loads pending is calibrated once the server has been idle
//     after an answer: /api/info reads ready with `calibrating`;
//   * a request that arrives during the calibration is not refused (no --autoload here, the way the
//     desktop and the apps run it): it stops the calibration, waits for the model and is answered;
//   * one attempt per load: a later answer does not calibrate again.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "server.h"

#include <cpp-httplib/httplib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace bmoe;
using namespace bmoe::server;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

static int failures = 0;

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

struct Scripted {
    std::atomic<int> opened{0};
    std::atomic<int> plans{0};
    std::atomic<int> calibrations{0};
    std::atomic<bool> calibrating{false};
    std::atomic<bool> stopped{false};
};

int free_port() {
    httplib::Server probe;
    const int port = probe.bind_to_any_port("127.0.0.1");
    return port;
}

bool wait_for(const std::function<bool()> & cond, std::chrono::milliseconds limit = 10s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return cond();
}

json info(httplib::Client & cli) {
    auto res = cli.Get("/api/info");
    if (!res || res->status != 200) return json::object();
    try {
        return json::parse(res->body);
    } catch (...) {
        return json::object();
    }
}

int chat(httplib::Client & cli) {
    const json body = {{"messages", json::array({{{"role", "user"}, {"content", "hello"}}})}, {"max_tokens", 4}};
    auto res = cli.Post("/v1/chat/completions", body.dump(), "application/json");
    return res ? res->status : -1;
}

} // namespace

int main() {
    const fs::path dir =
        fs::temp_directory_path() / ("bmoe-server-app-test-" + std::to_string((long long) ::time(nullptr)));
    fs::create_directories(dir / "models");
    const fs::path model = dir / "models" / "fake.gguf";
    {
        std::ofstream f(model, std::ios::binary);
        f << "not a real model";
    }

    Scripted script;
    Embedding emb;
    std::promise<std::function<void()>> stop_promise;
    std::atomic<bool> stop_set{false};
    emb.listening = [&](std::function<void()> stop) {
        if (stop && !stop_set.exchange(true)) stop_promise.set_value(stop);
    };
    emb.open_session = [&](const SessionConfig &, std::string &) -> std::unique_ptr<HostedSession> {
        ++script.opened;
        return std::make_unique<EchoSession>();
    };
    // A plan that leaves the device tier to time, as one beside a card of its own does.
    emb.plan = [&](const RunConfig & cfg) {
        ++script.plans;
        PlanOutcome p;
        p.ok = true;
        p.hardware = "0123456789abcdef";
        p.body = json{{"available", true}, {"model", cfg.model_path}};
        auto basis = std::make_shared<PlanBasis>();
        basis->cfg = cfg;
        basis->facts.plan.config = cfg;
        basis->facts.plan.config.trial.device_tier = 1;
        p.basis = basis;
        return p;
    };
    // A calibration that runs until a request stops it.
    emb.calibrate = [&](const PlanOutcome & p, const MeasuredKnobs &, const std::atomic<bool> & stop,
                        const PlanStepFn & on_step) {
        ++script.calibrations;
        script.calibrating = true;
        if (on_step) on_step("calibrate", "Timing the placements", 0, 4);
        for (int i = 0; i < 10000 && !stop.load(); ++i)
            std::this_thread::sleep_for(1ms);
        script.stopped = stop.load();
        script.calibrating = false;
        CalibrationOutcome c;
        c.ran = true;
        c.aborted = true;
        c.plan = p;
        return c;
    };
    emb.calibration_idle_delay = 50ms;

    const int port = free_port();
    const std::string port_s = std::to_string(port);
    const std::string dir_s = dir.u8string(), models_s = (dir / "models").u8string(), model_s = model.u8string();
    std::vector<std::string> args = {"bmoe-server", "--host",       "127.0.0.1", "--port",  port_s, "--data-dir",
                                     dir_s,         "--models-dir", models_s,    "--model", model_s};
    std::vector<char *> argv;
    for (std::string & a : args)
        argv.push_back(a.data());
    std::thread server([&] { run((int) argv.size(), argv.data(), emb); });

    auto stop_future = stop_promise.get_future();
    const bool listening = stop_future.wait_for(20s) == std::future_status::ready;
    check(listening, "the server listens");
    if (!listening) {
        server.detach();
        return 1;
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_read_timeout(20, 0);

    check(wait_for([&] {
              const json i = info(cli);
              return i.value("state", "") == "ready" && !i.value("calibrating", true);
          }),
          "the model named at launch is planned and loaded");
    check(script.plans == 1 && script.opened == 1, "one plan, one session");
    check(chat(cli) == 200, "it answers");

    check(wait_for([&] { return script.calibrating.load(); }), "idle after the answer, the pending side is calibrated");
    const json during = info(cli);
    check(during.value("state", "") == "ready" && during.value("calibrating", false),
          "during the calibration /api/info reads ready, and calibrating");

    // No --autoload: the request is not refused while the calibration holds the model, it stops it.
    check(chat(cli) == 200, "a request during the calibration is answered, not refused");
    check(script.stopped.load(), "and it stopped the calibration");
    check(wait_for([&] {
              const json i = info(cli);
              return i.value("state", "") == "ready" && !i.value("calibrating", true);
          }),
          "the model is loaded again after it");
    check(script.opened == 2, "one session opened after the calibration");

    check(chat(cli) == 200, "a later answer");
    std::this_thread::sleep_for(300ms);
    check(script.calibrations == 1, "one calibration per load: none after a later answer");

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
