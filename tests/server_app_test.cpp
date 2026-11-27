// bmoe-server end to end over HTTP, with the engine and the planner replaced by scripted ones
// (server_seams.h): the server's own policy on a real listening socket, no model. Started without
// --autoload, the way the desktop and the apps run it.
//
//   * a model named at launch is planned and loaded on the plan as made, the side it names included:
//     one open, no model opened to try a side, none reopened before or after the first answer;
//   * a load whose named side will not open opens the plan's other side instead, and the failure is
//     kept against the side, one more each time: a second failure reaches the count a plan drops the
//     side at (MeasuredKnobs::unviable_max);
//   * where the other side does not open either, the load ends in error and nothing is kept;
//   * a client that leaves while its turn is in the prefill stops that turn, on each of the four
//     endpoints that run one, streaming or not, and no other: the turn waiting behind it runs and
//     answers in full. One without a stream that leaves while it waits gives its place up at once,
//     while the turn before it still runs. A streaming client that only closes its sending side once
//     its stream has begun (a half-close) keeps the turn, and so do the turns of a connection kept
//     alive for a second request; without a stream a half-close counts as leaving, and its client
//     reads `499 client_gone`.
//
// Every wait is for an event (a state, a count, bytes read), never for time to pass.
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "listen_port.h"
#include "server.h"
#include "server_seams.h"
#include "stream_keepalive.h"

#include <cpp-httplib/httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// The scripted engine's turns, and what the test tells them: while `hold` is set, a turn waits in
// its prefill until the test lets one go or the turn's stop is raised, the engine's cancellation
// point as far as the server can see.
struct Turns {
    std::mutex m;
    std::condition_variable cv;
    bool hold = false;
    int let_go = 0;
    int started = 0, stopped = 0, answered = 0;

    template <class F> bool wait(F pred) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, k_ceiling, pred);
    }
    int count(const int & n) {
        std::lock_guard<std::mutex> lk(m);
        return n;
    }
    void set_hold(bool h) {
        std::lock_guard<std::mutex> lk(m);
        hold = h;
        cv.notify_all();
    }
    void release_one() {
        std::lock_guard<std::mutex> lk(m);
        ++let_go;
        cv.notify_all();
    }
};

// A session that answers "hi" to every turn.
class EchoSession final : public HostedSession {
public:
    explicit EchoSession(Turns & turns) : turns_(turns) {}

    RunResult generate(const GenerateRequest & gr,
                       const std::function<void(const TokenMetrics &)> & on_token) override {
        RunResult r;
        r.ok = true;
        {
            std::unique_lock<std::mutex> lk(turns_.m);
            ++turns_.started;
            turns_.cv.notify_all();
            while (turns_.hold && turns_.let_go == 0) {
                if (gr.stop && gr.stop->load()) {
                    ++turns_.stopped;
                    turns_.cv.notify_all();
                    r.cancelled = true;
                    return r;
                }
                // The stop is the caller's atomic, raised with no notice: looked at, as the engine does.
                turns_.cv.wait_for(lk, 5ms);
            }
            if (turns_.hold) --turns_.let_go;
        }
        TokenMetrics m;
        m.step = 1;
        m.steps = 1;
        m.piece = "hi";
        m.text = "hi";
        if (on_token) on_token(m);
        r.generated_text = "hi";
        r.summary.n_generated = 1;
        r.summary.s_per_token = 0.01;
        std::lock_guard<std::mutex> lk(turns_.m);
        ++turns_.answered;
        turns_.cv.notify_all();
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
    Turns & turns_;
    std::string arch_ = "fakemoe";
};

// A client on a socket of its own, for what an HTTP library's client does not do: leave while its
// request runs, or close only its sending side.
class RawClient {
public:
    explicit RawClient(int port) : s_(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short) port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (s_ != INVALID_SOCKET && ::connect(s_, (const sockaddr *) &a, sizeof a) != 0) close();
        if (s_ == INVALID_SOCKET) return;
#if defined(_WIN32)
        const DWORD ms = (DWORD) std::chrono::duration_cast<std::chrono::milliseconds>(k_ceiling).count();
        ::setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, (const char *) &ms, sizeof ms);
#else
        timeval tv{};
        tv.tv_sec = (decltype(tv.tv_sec)) k_ceiling.count();
        ::setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    }
    ~RawClient() { close(); }
    RawClient(const RawClient &) = delete;
    RawClient & operator=(const RawClient &) = delete;

    bool send(const std::string & bytes) {
        size_t off = 0;
        while (s_ != INVALID_SOCKET && off < bytes.size()) {
            const auto n = ::send(s_, bytes.data() + off, (int) (bytes.size() - off), 0);
            if (n <= 0) return false;
            off += (size_t) n;
        }
        return s_ != INVALID_SOCKET;
    }
    // A half-close: the request is sent, and the client still reads.
    void half_close() {
#if defined(_WIN32)
        ::shutdown(s_, SD_SEND);
#else
        ::shutdown(s_, SHUT_WR);
#endif
    }
    void close() {
        if (s_ == INVALID_SOCKET) return;
#if defined(_WIN32)
        ::closesocket(s_);
#else
        ::close(s_);
#endif
        s_ = INVALID_SOCKET;
    }
    // Reads until `done` holds for what has arrived; false when the connection ends (or the ceiling
    // passes) first.
    bool read_until(const std::function<bool(const std::string &)> & done) {
        char buf[4096];
        while (!done(got)) {
            if (s_ == INVALID_SOCKET) return false;
            const auto n = ::recv(s_, buf, (int) sizeof buf, 0);
            if (n <= 0) return false;
            got.append(buf, (size_t) n);
        }
        return true;
    }
    bool read_until(const std::string & marker) {
        return read_until([&](const std::string & g) { return g.find(marker) != std::string::npos; });
    }

    std::string got; // everything read so far

private:
    socket_t s_;
};

int occurrences(const std::string & s, const std::string & what) {
    int n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size()))
        ++n;
    return n;
}

// A POST as raw HTTP/1.1.
std::string http_post(const char * path, const json & body, const char * connection = "close") {
    const std::string b = body.dump();
    return std::string("POST ") + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n" +
           "Content-Length: " + std::to_string(b.size()) + "\r\nConnection: " + connection + "\r\n\r\n" + b;
}

const json k_hello = json::array({{{"role", "user"}, {"content", "hello"}}});

// A chat request as raw HTTP/1.1.
std::string chat_request(bool stream, const char * connection = "close") {
    return http_post("/v1/chat/completions", {{"messages", k_hello}, {"max_tokens", 4}, {"stream", stream}},
                     connection);
}

// The endpoints that run a chat turn, each with the last thing its stream sends before the turn's
// first token: the headers, or the protocol's opening frames.
struct TurnEndpoint {
    const char * path;
    json body;
    const char * opening;
};

std::vector<TurnEndpoint> turn_endpoints(bool stream) {
    std::vector<TurnEndpoint> e = {
        {"/v1/chat/completions", {{"messages", k_hello}, {"max_tokens", 4}}, "\r\n\r\n"},
        {"/v1/messages", {{"model", "m"}, {"messages", k_hello}, {"max_tokens", 4}}, "message_start"},
        {"/v1/responses", {{"model", "m"}, {"input", "hello"}, {"max_output_tokens", 4}}, "response.in_progress"},
        {"/v1/completions", {{"prompt", "hello"}, {"max_tokens", 4}}, "\r\n\r\n"},
    };
    for (TurnEndpoint & x : e)
        x.body["stream"] = stream;
    return e;
}

// Whether `got` holds all a stream sent up to `opening`, its chunk's end included: a client that has
// read it has nothing left unread, so its close is a plain end of stream rather than a reset.
bool has_opening(const std::string & got, const char * opening) {
    const auto ends = [&](const char * e) {
        const size_t n = std::strlen(e);
        return got.size() >= n && got.compare(got.size() - n, n, e) == 0;
    };
    return got.find(opening) != std::string::npos && (ends("\r\n\r\n") || ends("\n\n\r\n"));
}

// What the scripted engine and planner did, and what the test tells them to do next.
struct Script {
    std::mutex m;
    std::condition_variable cv;
    int opened = 0;        // sessions opened
    int open_failures = 0; // opens made to fail
    int fail_next_opens = 0;
    int plans = 0; // plans made

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

// Wait until `n` turns wait in line, as /metrics counts them (llama-server's requests_deferred); the
// line changes on the server's own threads.
bool wait_deferred(httplib::Client & cli, int n) {
    const std::string key = "\nllamacpp:requests_deferred ";
    const auto end = std::chrono::steady_clock::now() + k_ceiling;
    while (std::chrono::steady_clock::now() < end) {
        auto res = cli.Get("/metrics");
        const size_t at = res && res->status == 200 ? res->body.find(key) : std::string::npos;
        if (at != std::string::npos && std::atoi(res->body.c_str() + at + key.size()) == n) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

bool ready(const json & i) {
    return i.value("state", "") == "ready";
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
    const fs::path model = dir / "models" / "fake.gguf";
    {
        std::ofstream f(model, std::ios::binary);
        f << "not a real model " << model.filename().u8string();
    }

    Script sc;
    Turns turns;
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
        return std::make_unique<EchoSession>(turns);
    };
    // A plan that names a side of the device tier, as one beside a card of its own does; its own value
    // `trial-device-cache` tells the plan as made from the other side's.
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
        p.basis = basis;
        std::lock_guard<std::mutex> lk(sc.m);
        ++sc.plans;
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

    // ── a streaming client that leaves stops its own turn, in the prefill, on every streaming endpoint ──
    // What the stream sent is read first, so the close is a plain end of stream, not a reset: the first
    // keep-alive after it still goes out, and only the second fails.
    for (const TurnEndpoint & e : turn_endpoints(true)) {
        const int started = turns.count(turns.started), stopped = turns.count(turns.stopped);
        turns.set_hold(true);
        RawClient a(port);
        check(a.send(http_post(e.path, e.body)) && turns.wait([&] { return turns.started == started + 1; }) &&
                  a.read_until([&](const std::string & g) { return has_opening(g, e.opening); }),
              std::string(e.path) + ": a streamed turn is held in its prefill, its stream begun");
        a.close();
        check(turns.wait([&] { return turns.stopped == stopped + 1; }),
              std::string(e.path) + ": the client that leaves stops its turn inside the prefill");
        turns.set_hold(false);
    }

    // ── ... and so does one that does not stream: nothing is sent before its answer, so its close is a
    // plain end of stream as well ──
    for (const TurnEndpoint & e : turn_endpoints(false)) {
        const int started = turns.count(turns.started), stopped = turns.count(turns.stopped);
        turns.set_hold(true);
        RawClient a(port);
        check(a.send(http_post(e.path, e.body)) && turns.wait([&] { return turns.started == started + 1; }),
              std::string(e.path) + " without streaming: a turn is held in its prefill");
        a.close();
        check(turns.wait([&] { return turns.stopped == stopped + 1; }),
              std::string(e.path) + " without streaming: the client that leaves stops its turn inside the prefill");
        turns.set_hold(false);
    }

    // ── ... and no other: the turn waiting behind it runs ──
    for (const bool stream : {true, false}) {
        const std::string held = stream ? "a streamed turn" : "a turn without streaming";
        const int started = turns.count(turns.started), stopped = turns.count(turns.stopped);
        turns.set_hold(true);
        RawClient a(port), b(port);
        check(a.send(chat_request(stream)) && turns.wait([&] { return turns.started == started + 1; }),
              held + " is held in its prefill");
        check(b.send(chat_request(true)) && b.read_until(": waiting for a turn"), "a second waits behind it");
        if (stream)
            check(a.read_until([](const std::string & g) { return has_opening(g, "\r\n\r\n"); }),
                  "the held turn's stream has begun");
        a.close();
        check(turns.wait([&] { return turns.stopped == stopped + 1; }),
              "the client of " + held + " that leaves stops its turn inside the prefill");
        check(turns.wait([&] { return turns.started == started + 2; }), "and the turn waiting behind it runs");
        turns.release_one();
        check(b.read_until("data: [DONE]") && turns.count(turns.stopped) == stopped + 1,
              "it answers in full: the stop was the leaving client's alone");
        turns.set_hold(false);
    }

    // ── a client without a stream that leaves while it waits gives its place up at once ──
    {
        const int started = turns.count(turns.started), stopped = turns.count(turns.stopped);
        turns.set_hold(true);
        RawClient a(port), b(port);
        check(a.send(chat_request(true)) && turns.wait([&] { return turns.started == started + 1; }),
              "a streamed turn is held in its prefill");
        check(b.send(chat_request(false)) && wait_deferred(cli, 1), "a second, without streaming, waits behind it");
        b.close();
        check(wait_deferred(cli, 0) && turns.count(turns.started) == started + 1,
              "the waiting client that leaves gives its place up while the turn before it still runs");
        turns.release_one();
        check(a.read_until("data: [DONE]") && turns.count(turns.stopped) == stopped,
              "and the running turn answers in full");
        turns.set_hold(false);
    }

    // ── a streaming client that only half-closes keeps its turn (without a stream a half-close counts as
    // leaving, docs/server-api.md) ──
    {
        const int started = turns.count(turns.started), stopped = turns.count(turns.stopped);
        turns.set_hold(true);
        RawClient c(port);
        check(c.send(chat_request(true)) && turns.wait([&] { return turns.started == started + 1; }),
              "a streamed turn is held in its prefill");
        c.half_close();
        check(c.read_until([](const std::string & g) { return occurrences(g, ": ping") >= StreamKeepalive::kProbes; }),
              "a client that half-closes reads the keep-alives the closed-reading connection gets");
        turns.release_one();
        check(c.read_until("data: [DONE]") && turns.count(turns.stopped) == stopped,
              "and keeps its turn: the answer comes in full");
        turns.set_hold(false);
    }

    // ── without a stream nothing tells a half-close from a client that left: it counts as leaving, and
    // the client reads why ──
    {
        const int started = turns.count(turns.started), stopped = turns.count(turns.stopped);
        turns.set_hold(true);
        RawClient n(port);
        check(n.send(chat_request(false)) && turns.wait([&] { return turns.started == started + 1; }),
              "a turn without streaming is held in its prefill");
        n.half_close();
        check(turns.wait([&] { return turns.stopped == stopped + 1; }) && n.read_until([](const std::string & g) {
            return g.find("client_gone") != std::string::npos;
        }) && n.got.rfind("HTTP/1.1 499", 0) == 0,
              "a half-close without streaming stops its turn, answered 499 client_gone");
        turns.set_hold(false);
    }

    // ── a connection kept alive: neither request's turn is stopped ──
    {
        const int answered = turns.count(turns.answered), stopped = turns.count(turns.stopped);
        RawClient k(port);
        check(k.send(chat_request(true, "keep-alive")) && k.read_until("data: [DONE]") && k.read_until("\r\n0\r\n\r\n"),
              "a streamed answer on a connection kept alive");
        k.got.clear();
        check(k.send(chat_request(false)) && k.read_until([](const std::string & g) {
            return g.find("\"hi\"") != std::string::npos;
        }) && k.got.rfind("HTTP/1.1 200", 0) == 0,
              "then a second request on it");
        check(turns.count(turns.answered) == answered + 2 && turns.count(turns.stopped) == stopped,
              "both turns answered, none stopped");
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
