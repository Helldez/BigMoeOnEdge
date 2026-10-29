// The HTTP face of bmoe-server, and its composition root (server.h).
//
// Composes the engine (one EngineHost), the configuration (SettingsStore), the model files
// (ModelLibrary) and the event stream (EventBus) behind the HTTP API in docs/server-api.md, and
// serves the UI's static files. Like bmoe-cli it is a front-end: every engine tunable arrives
// through the parameter table, and nothing here knows a parameter by more than its key. Who may
// reach the API is decided in access.h; this file only reads the request and answers.
#include "server.h"

#include "access.h"
#include "agent.h"
#include "agent_routes.h"
#include "agent_store.h"
#include "anthropic_messages.h"
#include "connect.h"
#include "decide_protocol.h"
#include "engine_host.h"
#include "openai_chat.h"
#include "openai_responses.h"
#include "event_bus.h"
#include "feed.h"
#include "feed_key.h"
#include "hf_repo.h"
#include "http_fetch.h"
#include "json_util.h"
#include "model_inspect.h"
#include "model_library.h"
#include "plan_view.h"
#include "planner_adapter.h"
#include "platform.h"
#include "routing_store.h"
#include "settings_store.h"
#include "update_service.h"
#include "url_entry.h"

#include "bmoe/config.h"
#include "bmoe/params.h"
#include "bmoe/probe.h"
#include "bmoe/version.h"

#include <cpp-httplib/httplib.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace bmoe;
using namespace bmoe::server;

namespace {

constexpr int kPortMax = 65535;

constexpr const char * kDefaultHfEndpoint = "https://huggingface.co";
constexpr int kHfSearchLimit = 20; // repositories a search answers with: a screen's worth

// The server's workers. A request holds one for its whole life: an event stream for as long as a UI
// is open, a chat turn while it runs and while it waits its place in line. Those are bounded
// (k_max_event_streams, one running turn, k_max_waiting_turns waiting), and the pool is wider than
// all of them together, so a full set of streams and a full line still leave workers for the short
// requests (config, models, the UI's files) that keep the UI working. Seven: a browser opens six
// connections to one host, so a page load is served in one round, with one to spare. 32 in all.
constexpr size_t kShortRequestWorkers = 7;
constexpr size_t kWorkerThreads = k_max_event_streams + 1 + k_max_waiting_turns + kShortRequestWorkers;
// Connections accepted while every worker is busy. Past this the server closes the next one at once:
// a client that floods the port gets refused rather than an unbounded queue in this process.
constexpr size_t kMaxQueuedRequests = 64;
// How often an idle event stream is pinged: what keeps it open through a proxy that drops a silent
// connection, and the write that finds a client that stopped reading.
constexpr std::chrono::seconds kEventPingInterval{15};
// How often an idle event stream looks for its client. A stream's place is one of a few, and a UI
// that reloads takes a new one at once: a client that closed its connection must not hold the old
// one until the next ping. The same second a waiting chat turn looks with (TurnQueue::enter).
constexpr std::chrono::seconds kEventLivenessPoll{1};
// How often a server started by an app (--parent-pid) looks for it.
constexpr std::chrono::seconds kParentPollInterval{1};
// A client that vanishes without closing (a phone that drops off Wi-Fi) is invisible to the checks
// above: nothing comes back to read, and pings that small never fill the send buffer, so without a
// bound from TCP its stream would keep one of the few places until the stack's own retransmission
// limit, many minutes. These bound it:
// - unacknowledged data is given up after a minute: longer than the stall of a phone moving from
//   Wi-Fi to cellular, short enough that the place is free before a person reloads twice;
constexpr int kTcpUnackedTimeoutS = 60;
// - a connection with nothing in flight is probed after the ping interval, every 5 s, 3 times.
constexpr int kTcpKeepAliveIdleS = 15;
constexpr int kTcpKeepAliveIntervalS = 5;
constexpr int kTcpKeepAliveProbes = 3;

// Set on the listening socket, the one place the library offers (svr.set_socket_options). A stack
// that copies a listener's TCP options to the connections it accepts applies them to every client;
// one that does not keeps its defaults for the options it does not copy. Each option is set where
// the system's headers define it: what the stack can do, not which system it is.
void set_listener_options(socket_t sock) {
    httplib::default_socket_options(sock); // what replacing the library's default must keep
    httplib::set_socket_opt(sock, SOL_SOCKET, SO_KEEPALIVE, 1);
#if defined(TCP_KEEPIDLE)
    httplib::set_socket_opt(sock, IPPROTO_TCP, TCP_KEEPIDLE, kTcpKeepAliveIdleS);
#elif defined(TCP_KEEPALIVE)
    httplib::set_socket_opt(sock, IPPROTO_TCP, TCP_KEEPALIVE, kTcpKeepAliveIdleS); // the older name, seconds
#endif
#if defined(TCP_KEEPINTVL)
    httplib::set_socket_opt(sock, IPPROTO_TCP, TCP_KEEPINTVL, kTcpKeepAliveIntervalS);
#endif
#if defined(TCP_KEEPCNT)
    httplib::set_socket_opt(sock, IPPROTO_TCP, TCP_KEEPCNT, kTcpKeepAliveProbes);
#endif
#if defined(TCP_USER_TIMEOUT)
    httplib::set_socket_opt(sock, IPPROTO_TCP, TCP_USER_TIMEOUT, kTcpUnackedTimeoutS * 1000); // milliseconds
#elif defined(TCP_RXT_CONNDROPTIME)
    httplib::set_socket_opt(sock, IPPROTO_TCP, TCP_RXT_CONNDROPTIME, kTcpUnackedTimeoutS);
#elif defined(TCP_MAXRT)
    httplib::set_socket_opt(sock, IPPROTO_TCP, TCP_MAXRT, kTcpUnackedTimeoutS);
#endif
}

struct Options {
    std::string host = kDefaultHost;
    int port = kDefaultPort;
    std::string ui_dir, models_dir, data_dir, catalog;
    std::string skills_dir;             // the agent's bundled skills; default skills/ beside this binary
    std::string mcp_config;             // the MCP servers' config; default <data>/mcp.json
    std::vector<std::string> scan_dirs; // more directories whose models are listed, never written
    std::vector<std::string> allow_origins;
    std::string api_key;
    std::string api_key_file; // --api-key-file: the key, read at start, never on the command line
    bool no_api_key = false;  // listen beyond loopback with no key, on purpose
    bool models_only = false; // every client, local ones too, names models in the models folder only
    bool chat_only = false;   // conversations are plain chat: no agent tools, workspaces or tasks
    bool open_browser = false;
    bool isolated_probes = true;                  // the planner asks each device in a child process first
    std::string hf_endpoint = kDefaultHfEndpoint; // where repositories are listed and fetched from
    std::string feed_url = kFeedUrl;              // the release feed, read only when a person asks
    long parent_pid = 0;                          // --parent-pid: stop when that process is gone
    std::string probe_device;                     // internal: this process IS such a child; probe this device and exit
    std::map<std::string, std::string> launch;    // engine parameters given on the command line
};

// What a chat front-end wants when nobody has said otherwise. These are the server's defaults, not
// the engine's: the library keeps its own (an embedder's 0 still means "no cache"), and the CLI
// resolves its own the same way.
RunConfig server_defaults() {
    RunConfig c;
    c.chatml = true;         // a chat UI always renders the model's own chat template
    c.moe.enabled = true;    // this is the engine the UI exists for; a dense model turns it off at load
    c.moe.cache_auto = true; // with streaming on, no cache re-reads every expert every token (#186)
    c.n_ctx = kChatContextTokens;
    c.n_predict = kChatAnswerTokens;
    c.decide.enabled = true; // POST /api/decide; costs nothing until a decision is asked for
    // Byte-identical and the measured best on every device; needs the fork's hook in the CPU
    // backend, which is a fact about the backend loaded, asked of it rather than assumed.
    c.moe.overlap = expert_ready_hook_available();
    return c;
}

void print_usage(const char * argv0) {
    std::printf("usage: %s [options] [engine parameters]\n"
                "\n"
                "  --host ADDR          address to bind (default %s; anything else exposes the engine)\n"
                "  --port N             port (default %d)\n"
                "  --open               open the UI in the default browser once listening\n"
                "  --models-dir PATH    where models are listed and downloaded (default: <data dir>/models)\n"
                "  --scan-dir PATH      also list the models in PATH and in the folders directly inside it,\n"
                "                       loaded in place (repeatable; one level deep, links to folders skipped)\n"
                "  --data-dir PATH      settings and models (default: the per-user application data folder)\n"
                "  --ui-dir PATH        the built web UI (default: ui/ beside this binary)\n"
                "  --catalog PATH       the model catalog (default: catalog/models.json beside this binary);\n"
                "                       the client catalog (clients.json) is read from the same folder\n"
                "  --skills-dir PATH    the agent's bundled skills (default: skills/ beside this binary)\n"
                "  --mcp-servers-config PATH\n"
                "                       the MCP servers whose tools the agent offers, as {\"mcpServers\": ...}\n"
                "                       (default: mcp.json in the data folder, when it exists)\n"
                "  --allow-origin URL   also accept requests from this browser origin (a UI dev server)\n"
                "  --api-key KEY        require this key on the API (Authorization: Bearer KEY); needed\n"
                "                       with a --host beyond loopback. Other local users can read a\n"
                "                       command line: prefer --api-key-file\n"
                "  --api-key-file PATH  the same, with the key read from PATH (trailing newline ignored)\n"
                "  --no-api-key         listen beyond loopback with no key, on a network you trust\n"
                "  --models-only        local clients too may only open models from the folders models\n"
                "                       are listed from (clients on the network always are)\n"
                "  --chat-only          conversations are plain chat: no agent tools, workspaces or tasks\n"
                "  --no-isolated-probes the planner probes compute devices in this process instead of a child each\n"
                "  --hf-endpoint URL    where Hugging Face repositories are read from (default %s; a mirror)\n"
                "  --feed-url URL       the signed release feed backend packs and updates are read from\n"
                "                       (https; default: this project's latest release)\n"
                "  --parent-pid PID     stop when process PID ends (the app that started this server)\n"
                "  -h, --help           this text\n"
                "      --version        print the engine version\n"
                "\n"
                "Engine parameters are the bmoe-cli flags (bmoe-cli --help lists them). Given here, they\n"
                "hold for this run and outrank saved settings; -m loads that model at start.\n",
                argv0, kDefaultHost, kDefaultPort, kDefaultHfEndpoint);
}

// -1: continue; otherwise the exit code.
int parse_args(int argc, char ** argv, Options & o) {
    RunConfig scratch;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--host")
            o.host = next();
        else if (a == "--port") {
            long long port = 0;
            std::string err;
            if (!parse_int_strict(next(), 1, kPortMax, port, err)) {
                std::fprintf(stderr, "bmoe-server: --port %s\n", err.c_str());
                return 2;
            }
            o.port = (int) port;
        } else if (a == "--open")
            o.open_browser = true;
        else if (a == "--models-dir")
            o.models_dir = next();
        else if (a == "--scan-dir")
            o.scan_dirs.push_back(next());
        else if (a == "--data-dir")
            o.data_dir = next();
        else if (a == "--ui-dir")
            o.ui_dir = next();
        else if (a == "--catalog")
            o.catalog = next();
        else if (a == "--skills-dir")
            o.skills_dir = next();
        else if (a == "--mcp-servers-config")
            o.mcp_config = next();
        else if (a == "--allow-origin")
            o.allow_origins.push_back(next());
        else if (a == "--api-key")
            o.api_key = next();
        else if (a == "--api-key-file")
            o.api_key_file = next();
        else if (a == "--no-api-key")
            o.no_api_key = true;
        else if (a == "--models-only")
            o.models_only = true;
        else if (a == "--chat-only")
            o.chat_only = true;
        else if (a == "--no-isolated-probes")
            o.isolated_probes = false;
        else if (a == "--probe-device")
            o.probe_device = next();
        else if (a == "--hf-endpoint") {
            o.hf_endpoint = next();
            while (!o.hf_endpoint.empty() && o.hf_endpoint.back() == '/')
                o.hf_endpoint.pop_back();
            if (o.hf_endpoint.compare(0, 8, "https://") != 0 && o.hf_endpoint.compare(0, 7, "http://") != 0) {
                std::fprintf(stderr, "bmoe-server: --hf-endpoint must be an http(s) URL\n");
                return 2;
            }
        } else if (a == "--parent-pid") {
            long long pid = 0;
            std::string err;
            if (!parse_int_strict(next(), 1, std::numeric_limits<int>::max(), pid, err)) {
                std::fprintf(stderr, "bmoe-server: --parent-pid %s\n", err.c_str());
                return 2;
            }
            o.parent_pid = (long) pid;
        } else if (a == "--feed-url") {
            o.feed_url = next();
            if (o.feed_url.compare(0, 8, "https://") != 0) {
                std::fprintf(stderr, "bmoe-server: --feed-url must be an https URL\n");
                return 2;
            }
        } else if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (a == "--version") {
            std::printf("%s\n", version());
            return 0;
        } else {
            const FlagResult r = apply_flag(scratch, a.c_str(), i + 1 < argc ? argv[i + 1] : nullptr);
            if (!r.matched) {
                std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
                print_usage(argv[0]);
                return 1;
            }
            if (!r.error.empty()) {
                std::fprintf(stderr, "bmoe-server: %s\n", r.error.c_str());
                return 2;
            }
            if (r.consumed_value) ++i;
            o.launch[r.param->key] = r.param->get(scratch);
        }
    }
    if (!o.api_key_file.empty()) {
        if (!o.api_key.empty()) {
            std::fprintf(stderr, "bmoe-server: --api-key and --api-key-file are alternatives; choose one\n");
            return 2;
        }
        std::string err;
        if (!read_api_key_file(o.api_key_file, o.api_key, err)) {
            std::fprintf(stderr, "bmoe-server: %s\n", err.c_str());
            return 2;
        }
    }
    return -1;
}

// The first candidate that exists on disk, or "".
std::string first_existing(const std::vector<fs::path> & candidates) {
    std::error_code ec;
    for (const fs::path & p : candidates)
        if (!p.empty() && fs::exists(p, ec)) return p.u8string();
    return "";
}

void send_json(httplib::Response & res, const json & j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
}

// `error` is for a person; `code`, when the failure has one, is what a client decides on.
void send_error(httplib::Response & res, int status, const std::string & msg, const std::string & code = "") {
    json body{{"error", msg}};
    if (!code.empty()) body["code"] = code;
    send_json(res, body, status);
}

bool is_messages_path(const std::string & path) {
    return path == "/v1/messages" || path.rfind("/v1/messages/", 0) == 0;
}
bool is_responses_path(const std::string & path) {
    return path == "/v1/responses" || path.rfind("/v1/responses/", 0) == 0;
}

// The error body the protocol of `path` defines: an Anthropic client and a Responses client each
// read only their own shape, from the refusals before routing as much as from a turn. Every other
// path answers in this API's.
void send_path_error(const std::string & path,
                     httplib::Response & res,
                     int status,
                     const std::string & msg,
                     const std::string & code = "") {
    if (is_messages_path(path)) return send_json(res, anthropic_error_json(status, msg, code), status);
    if (is_responses_path(path)) return send_json(res, responses_error_json(status, msg, code), status);
    send_error(res, status, msg, code);
}

// Every body this API reads is an object; anything else is refused here, so no handler reads a
// field out of an array or a number.
bool parse_body(const httplib::Request & req, httplib::Response & res, json & out) {
    if (req.body.empty()) {
        out = json::object();
        return true;
    }
    try {
        out = json::parse(req.body);
    } catch (const std::exception & e) {
        send_path_error(req.path, res, 400, std::string("invalid JSON body: ") + e.what(), "invalid_request");
        return false;
    }
    if (!out.is_object()) {
        send_path_error(req.path, res, 400, "the request body must be a JSON object", "invalid_request");
        return false;
    }
    return true;
}

bool starts_with(const std::string & s, const std::string & prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

struct App {
    Options opt;
    EventBus bus;
    SettingsStore store;
    EngineHost host;
    ModelLibrary lib;
    std::atomic<long long> chat_seq{0};

    // The plan last shown, so "apply" applies what the person saw: the probes measure a live
    // machine, and a second run could plan differently.
    std::mutex plan_m;
    // One measurement at a time. The probes own the machine while they run - every core, and
    // gigabytes of uncached reads - so two at once measure each other, and nothing below them was
    // written to be entered twice.
    std::mutex measure_m;
    // One load at a time, from "unload the old model" to "the loader has the new one": a second
    // request waits its turn instead of planning for a machine the first is still changing.
    std::mutex load_m;
    PlanOutcome last_plan;
    // The plan the loaded model runs with, made on a quiet machine by the last auto load. Kept apart
    // from last_plan: a plan measured by hand with that model resident is a different, pessimistic
    // measurement and must not replace it.
    PlanOutcome load_plan;
    std::atomic<uint64_t> plan_seq{0};

    // The network, for reading a repository (http_fetch.h), and where its bytes in flight go.
    std::unique_ptr<HttpFetch> net;
    std::string scratch_dir;
    // The release feed, backend packs and updates (update_service.h); made once the backends are
    // known.
    std::unique_ptr<UpdateService> updates;

    App(Options o,
        std::string settings_path,
        std::string models_dir,
        std::string catalog,
        std::string added,
        std::string scratch,
        std::string routing_dir,
        const std::function<std::unique_ptr<HttpFetch>(const std::string &)> & fetch)
        : opt(std::move(o)), store(server_defaults(), std::move(settings_path)), host(bus),
          lib(std::move(models_dir), opt.scan_dirs, std::move(catalog), std::move(added), opt.hf_endpoint, bus),
          net(fetch ? fetch(scratch) : curl_fetch(scratch, opt.hf_endpoint)), scratch_dir(std::move(scratch)),
          routing(std::move(routing_dir)) {
        host.set_routing_sink(
            [this](const std::string & model, const RoutingTrace & rec) { routing.keep(model, rec); });
        // A verdict is kept against the machine the plan of that model measured; with no plan of it
        // there is no machine to name, and it is not kept.
        host.set_knobs_sink([this](const std::string & model, const MeasuredKnobs & mk) {
            MeasuredKnobs rec = mk;
            {
                std::lock_guard<std::mutex> lk(plan_m);
                for (const PlanOutcome * p : {&load_plan, &last_plan})
                    if (p->ok && p->body.value("model", "") == model && !p->hardware.empty()) {
                        rec.hardware = p->hardware;
                        break;
                    }
            }
            if (rec.hardware.empty()) return;
            rec.engine = version();
            routing.keep_knobs(model, rec);
        });
    }

    // Each model's recorded routing, kept for the next plan of that file (routing_store.h).
    RoutingStore routing;

    // The agent (docs/agent.md): what it keeps, its runs and scheduler, and what its routes reach.
    // Declared after the host, so it is destroyed, and its runs stopped and joined, before the host.
    std::unique_ptr<agent::AgentStore> agent_store;
    std::unique_ptr<McpHub> mcp; // before the agent, so it outlives it
    std::unique_ptr<agent::Agent> agent;
    AgentRouteDeps agent_routes;

    json config_object(const json * rejected = nullptr) {
        const RunConfig cfg = store.config();
        const ValidationResult v = validate(cfg);
        const std::vector<std::string> reload = host.reload_keys(cfg);
        json o = {{"values", config_values(cfg)},
                  {"user_keys", store.operator_keys()},
                  {"launch_keys", store.launch_keys()},
                  {"args", to_args(cfg)},
                  {"valid", v.ok},
                  {"error", engine_message(v.error)},
                  {"reload_required", !reload.empty()},
                  {"reload_keys", reload},
                  {"plan", store.plan_decisions()},
                  {"auto_plan", auto_plan()}};
        if (rejected) o["rejected"] = *rejected;
        return o;
    }

    void publish_config() { bus.publish("config", config_object()); }

    // A measurement, with each stage announced as it starts (`plan_progress` events) and its end, so
    // a UI says which probe is running rather than waiting blind. The caller holds measure_m.
    PlanOutcome measure_plan() {
        const RunConfig cfg = store.planning_config();
        RoutingTrace kept;
        const bool have = routing.load(cfg.model_path, kept);
        // The knob verdicts kept for this model; a plan here always lets the first answer measure
        // what they do not cover, since the verdict has a place to be kept.
        std::vector<MeasuredKnobs> knobs;
        const bool have_knobs = routing.load_knobs(cfg.model_path, knobs);
        PlanOutcome p = make_plan(
            cfg, store.operator_keys(), opt.isolated_probes,
            [this](const std::string & step, const std::string & label, int index, int count) {
                bus.publish("plan_progress",
                            json{{"step", step}, {"label", label}, {"index", index}, {"count", count}});
            },
            have ? &kept : nullptr, have_knobs ? &knobs : nullptr, /*measure_knobs*/ true);
        bus.publish("plan_progress", json{{"step", "done"}});
        p.seq = ++plan_seq;
        return p;
    }

    // A client on the network names a model in a folder this server lists models from (the models
    // folder, the --scan-dir ones) and nowhere else: it may not have the server open any file its
    // user can read. A local client is the person at this machine, as
    // the access rules have it, unless --models-only confines it too.
    bool model_path_for(const httplib::Request & req, const json & v, json & out, std::string & err) const {
        out = v;
        if (!v.is_string() || (!opt.models_only && is_loopback_peer(req.remote_addr))) return true;
        std::string path;
        if (!confine_model_path(v.get<std::string>(), lib.model_roots(), path, err)) return false;
        out = path;
        return true;
    }

    // The model loaded now and what its plan's ledger says it holds: the memory a load of another
    // model gets back, since a load unloads first. Only the plan this session was loaded with
    // counts; one measured by hand afterwards, or a load with no plan, gives no account (0).
    LoadedSession loaded_session() {
        LoadedSession s;
        if (!host.loaded_or_loading()) return s;
        const std::string path = host.model_path();
        s.model = host.model_name();
        std::lock_guard<std::mutex> lk(plan_m);
        if (load_plan.ok && !path.empty() && load_plan.body.value("model", "") == path)
            s.held_bytes = load_plan.held_bytes;
        return s;
    }

    // On by default wherever there is a planner to ask: the point of measuring the machine is that
    // nobody has to tune it by hand. A person can turn it off and it stays off.
    bool auto_plan() const { return planner_available() && store.auto_plan(true); }

    // Load the configured model.
    //
    // In auto mode the plan is made first, and on a quiet machine: the session being replaced goes
    // before the probes, because its memory would otherwise read as taken and the plan would size
    // everything for a machine that is not this one. Values a person set stay pinned.
    //
    // A model this build does not stream (a dense one) cannot open with streaming on, so streaming
    // follows the model unless the user set it themselves.
    std::string load_current() {
        if (store.config().model_path.empty()) return "no model selected";
        std::lock_guard<std::mutex> load(load_m);
        if (auto_plan()) {
            host.unload();
            host.begin_planning();
            std::unique_lock<std::mutex> measuring(measure_m);
            PlanOutcome p = measure_plan();
            measuring.unlock();
            if (p.ok) {
                store.apply_plan(p.values, p.decisions);
                std::lock_guard<std::mutex> lk(plan_m);
                load_plan = p;
                last_plan = std::move(p);
            }
            publish_config();
        } else {
            // A load on the settings as they are runs no plan: an earlier auto load's must not be
            // answered as the one this session runs with.
            std::lock_guard<std::mutex> lk(plan_m);
            load_plan = PlanOutcome{};
        }
        // The planning flag is cleared after the load has been asked for, so the state goes from
        // planning straight to loading (or to the error) with no empty in between.
        struct EndPlanning {
            EngineHost & h;
            ~EndPlanning() { h.end_planning(); }
        } end_planning{host};
        const RunConfig requested = store.config();
        RunConfig effective = requested;
        const std::vector<std::string> ops = store.operator_keys();
        const bool pinned = std::find(ops.begin(), ops.end(), "moe-stream") != ops.end();
        // A readable file with no routed experts this engine streams runs as plain mmap, unless the
        // operator pinned streaming on, in which case the engine refuses the load with its reason.
        const ModelLibrary::ModelFacts facts = lib.probe(requested.model_path);
        if (effective.moe.enabled && !pinned && !facts.arch.empty() && !facts.streamable) effective.moe.enabled = false;
        if (!effective.moe.enabled) drop_streaming_only(effective);
        return host.load(effective, requested);
    }

    // Everything that only means something on a running stream, turned off with it. A default the
    // server arms (overlap) or a plan that declines streaming must not leave a config validate()
    // refuses; the knobs come back on their own the next time streaming does.
    static void drop_streaming_only(RunConfig & c) {
        c.moe.overlap = false;
        c.moe.io_two_wave = false;
        c.moe.predict_log = false;
        c.moe.predict_prefetch = false;
        c.moe.route_ahead = 0;
        c.moe.row_stream = false;
        c.trial.overlap = false;
    }
};

// ── a model from a Hugging Face repository (hf_repo.h) ──
// Three steps, each a request the UI makes in turn: list what a repository holds, inspect one
// model's header against this machine, add it to the catalog. The download is then the catalog's,
// by id, as for every other entry. The server fetches only from its endpoint, with a repository
// id and paths it validated; a token a person pastes is used for the request and kept nowhere.
struct HfRequest {
    HfRef ref;
    std::string name, token;
};

// The request's repository, model name and token; false with the response already sent.
bool read_hf_request(const httplib::Request & req, httplib::Response & res, HfRequest & out, bool need_name) {
    json body;
    if (!parse_body(req, res, body)) return false;
    if (!body.contains("repo") || !body["repo"].is_string()) {
        send_error(res, 400, "repo must be a repository id or URL string", "invalid_request");
        return false;
    }
    std::string err;
    if (!parse_hf_ref(body["repo"].get<std::string>(), out.ref, err)) {
        // Its own code: a UI falls back to a search on it, and on nothing else.
        send_error(res, 400, err, "not_a_repo");
        return false;
    }
    if (body.contains("token")) {
        if (!body["token"].is_string()) {
            send_error(res, 400, "token must be a string", "invalid_request");
            return false;
        }
        out.token = body["token"].get<std::string>();
    }
    if (body.contains("name") && body["name"].is_string()) out.name = body["name"].get<std::string>();
    if (out.name.empty()) out.name = out.ref.file;
    if (need_name && out.name.empty()) {
        send_error(res, 400, "name must be the model's first file in the repository", "invalid_request");
        return false;
    }
    return true;
}

// The repository's models, fetched and parsed; false with the response already sent.
bool list_hf_models(App & app, httplib::Response & res, const HfRequest & r, std::vector<HfModel> & models) {
    std::string body, err;
    if (!app.net->get(hf_tree_url(app.opt.hf_endpoint, r.ref), 0, 0, r.token, body, err)) {
        send_error(res, 502, "could not read " + r.ref.repo + ": " + err, "upstream");
        return false;
    }
    json tree;
    try {
        tree = json::parse(body);
    } catch (const std::exception &) {
        send_error(res, 502, "the listing of " + r.ref.repo + " is not JSON", "upstream");
        return false;
    }
    if (!parse_hf_tree(tree, models, err)) {
        send_error(res, 404, r.ref.repo + ": " + err, "not_found");
        return false;
    }
    return true;
}

bool find_hf_model(httplib::Response & res, const HfRequest & r, const std::vector<HfModel> & models, HfModel & out) {
    for (const HfModel & m : models)
        if (m.name == r.name) {
            out = m;
            return true;
        }
    send_error(res, 404, r.ref.repo + " has no model named " + r.name, "not_found");
    return false;
}

json hf_model_json(const HfModel & m) {
    json files = json::array();
    for (const HfFile & f : m.files)
        files.push_back({{"path", f.path}, {"bytes", f.bytes}});
    return json{{"name", m.name},   {"quant", m.quant},         {"bytes", m.bytes}, {"role", m.role},
                {"count", m.count}, {"complete", m.complete()}, {"files", files}};
}

// One turn of an agent protocol (Anthropic Messages, OpenAI Responses): the chat endpoint's turn,
// answered whole through `answer` or streamed through `Stream` (AnthropicStream, ResponsesStream),
// whose frames are append-only (agent_protocol.h). `keepalive` is what the protocol writes while
// the turn waits its place in line.
template <class Stream, class Answer>
void agent_turn(App & app,
                const httplib::Request & req,
                httplib::Response & res,
                const ChatRequest & turn,
                bool stream,
                const ChatResponseMeta & meta,
                Answer answer,
                const std::string & keepalive) {
    if (!stream) {
        ChatOutcome out = app.host.chat(turn, nullptr, [&req] { return !req.is_connection_closed(); });
        if (out.status != 200) return send_path_error(req.path, res, out.status, out.error, out.code);
        ensure_tool_call_ids(out.result.tool_calls, meta.id);
        return send_json(res, answer(meta, out.result, finish_reason(out.result, out.n_predict)));
    }
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider("text/event-stream",
                                     [&app, turn, meta, keepalive](size_t, httplib::DataSink & sink) {
                                         // As on the chat endpoint, a failed write marks the client gone, and stopping
                                         // the work is the host's, through on_token's answer.
                                         bool alive = true;
                                         auto write = [&](const std::string & frames) {
                                             if (alive && !frames.empty() && !sink.write(frames.data(), frames.size()))
                                                 alive = false;
                                         };
                                         Stream s(meta);
                                         write(s.start());
                                         ChatOutcome out = app.host.chat(
                                             turn,
                                             [&](const TokenMetrics & m) {
                                                 if (alive) write(s.on_token(m.text, m.reasoning));
                                                 return alive;
                                             },
                                             [&] {
                                                 write(keepalive);
                                                 return alive;
                                             });
                                         if (out.status != 200) {
                                             write(s.error(out.status, out.error, out.code));
                                         } else {
                                             ensure_tool_call_ids(out.result.tool_calls, meta.id);
                                             write(s.finish(out.result, finish_reason(out.result, out.n_predict)));
                                         }
                                         sink.done();
                                         return true;
                                     });
}

void register_routes(httplib::Server & svr, App & app) {
    svr.Get("/api/info", [&](const httplib::Request &, httplib::Response & res) { send_json(res, app.host.info()); });

    svr.Get("/api/params", [&](const httplib::Request &, httplib::Response & res) {
        res.set_content(params_json(app.store.defaults()), "application/json");
    });

    svr.Get("/api/config",
            [&](const httplib::Request &, httplib::Response & res) { send_json(res, app.config_object()); });

    svr.Put("/api/config", [&](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        const json values = body.value("values", json::object());
        const json reset = body.value("reset", json::array());
        if (!values.is_object() || !reset.is_array())
            return send_error(res, 400, "values must be an object and reset an array");
        if (body.contains("auto_plan")) {
            if (!body["auto_plan"].is_boolean()) return send_error(res, 400, "auto_plan must be a boolean");
            app.store.set_auto_plan(body["auto_plan"].get<bool>());
        }
        std::vector<std::string> reset_keys;
        for (const json & k : reset)
            if (k.is_string()) reset_keys.push_back(k.get<std::string>());
        // A path the engine WRITES to is not a setting a client may choose: whoever can reach this
        // port could otherwise have the server create or append to any file its user can. The model
        // path is the one path a client names, and it is only ever read.
        json accepted = json::object();
        json refused = json::object();
        for (auto it = values.begin(); it != values.end(); ++it) {
            const ParamDesc * d = find_param(it.key());
            std::string why;
            json model;
            if (d && d->type == ParamType::Path && it.key() != "model")
                refused[it.key()] = "a file path other than the model is set at launch, not over the API";
            else if (it.key() == "model" && !app.model_path_for(req, it.value(), model, why))
                refused[it.key()] = why;
            else
                accepted[it.key()] = it.key() == "model" ? model : it.value();
        }
        json rejected = app.store.apply(accepted, reset_keys);
        rejected.update(refused);
        send_json(res, app.config_object(&rejected));
        app.publish_config();
    });

    svr.Post("/api/session/load", [&](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        if (body.contains("model")) {
            if (!body["model"].is_string()) return send_error(res, 400, "model must be a path string");
            json model;
            std::string why;
            if (!app.model_path_for(req, body["model"], model, why))
                return send_error(res, 403, why, "outside_models_dir");
            app.store.apply(json{{"model", model}}, {});
            app.publish_config();
        }
        const std::string err = app.load_current();
        if (!err.empty()) return send_error(res, 400, err);
        send_json(res, app.host.info(), 202);
    });

    svr.Post("/api/session/unload", [&](const httplib::Request &, httplib::Response & res) {
        app.host.unload();
        send_json(res, app.host.info());
        app.publish_config();
    });

    svr.Post("/api/cancel", [&](const httplib::Request &, httplib::Response & res) {
        app.host.cancel();
        send_json(res, json{{"ok", true}});
    });

    svr.Get("/v1/models", [&](const httplib::Request &, httplib::Response & res) {
        json data = json::array();
        const std::string name = app.host.model_name();
        if (!name.empty()) data.push_back({{"id", name}, {"object", "model"}, {"owned_by", "local"}});
        send_json(res, json{{"object", "list"}, {"data", data}});
    });

    svr.Post("/v1/chat/completions", [&](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        OpenAiChatRequest cr;
        std::string err;
        if (!parse_chat_request(body, app.store.config(), cr, err)) return send_error(res, 400, err, "invalid_request");
        if (!app.host.ready())
            return send_error(res, 503, "no model is loaded: load one from the Models page", error_code::no_model);

        ChatResponseMeta meta;
        meta.id = "chatcmpl-" + std::to_string(++app.chat_seq);
        meta.created = (long long) std::time(nullptr);
        meta.model = app.host.model_name();
        const auto dump = [](const json & j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };

        if (!cr.stream) {
            // Waiting for a turn, the only sign of a client that left is its closed connection.
            ChatOutcome out = app.host.chat(cr.turn, nullptr, [&req] { return !req.is_connection_closed(); });
            if (out.status != 200) return send_error(res, out.status, out.error, out.code);
            ensure_tool_call_ids(out.result.tool_calls, meta.id);
            return send_json(res, completion_json(meta, out.result, finish_reason(out.result, out.n_predict)));
        }

        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider("text/event-stream", [&app, cr, meta, dump](size_t, httplib::DataSink & sink) {
            // A failed write marks the client gone. Stopping the work is the host's, through
            // on_token's answer: this handler cannot tell whose turn is running when a write fails.
            bool alive = true;
            auto write = [&](const std::string & frame) {
                if (alive && !sink.write(frame.data(), frame.size())) alive = false;
            };
            auto chunk = [&](const json & delta, const json & finish, const json & extra) {
                json c = chunk_json(meta, delta, finish);
                if (!extra.is_null()) c["bmoe"] = extra;
                write("data: " + dump(c) + "\n\n");
            };

            // Deltas, as the CLI's line protocol sends them: append the new tail; when the chat
            // parser reclassified text already sent (a closing reasoning tag), send the full state
            // with reset so the client replaces instead of appending.
            std::string sent_text, sent_reasoning;
            const auto stream_text = [&](const std::string & text) {
                // A JSON string value may contain a code point split across byte-fallback tokens.
                // Serializing an incomplete one with replacement would corrupt the final object.
                return cr.turn.json_schema.empty() ? text : text.substr(0, complete_utf8_prefix_size(text));
            };
            bool first = true;
            ChatOutcome out = app.host.chat(
                cr.turn,
                [&](const TokenMetrics & m) {
                    if (!alive) return false;
                    json delta = json::object();
                    json extra = nullptr;
                    const std::string text = stream_text(m.text), reasoning = stream_text(m.reasoning);
                    if (first) delta["role"] = "assistant";
                    if (starts_with(text, sent_text) && starts_with(reasoning, sent_reasoning)) {
                        const std::string dt = text.substr(sent_text.size());
                        const std::string dr = reasoning.substr(sent_reasoning.size());
                        if (dt.empty() && dr.empty() && !first) return true;
                        if (!dt.empty()) delta["content"] = dt;
                        if (!dr.empty()) delta["reasoning_content"] = dr;
                    } else {
                        delta["content"] = text;
                        delta["reasoning_content"] = reasoning;
                        extra = json{{"reset", true}};
                    }
                    first = false;
                    sent_text = text;
                    sent_reasoning = reasoning;
                    chunk(delta, nullptr, extra);
                    return alive;
                },
                // Waiting for a turn: an SSE comment keeps the connection open through proxies
                // that time out a silent one, and a failed write is how a client that left is seen.
                [&] {
                    write(": waiting for a turn\n\n");
                    return alive;
                });
            if (out.status != 200) {
                write("data: " + dump(json{{"error", {{"message", out.error}, {"code", out.code}}}}) + "\n\n");
            } else {
                // The per-token text can lag the final answer (the chat parser holds back what it
                // cannot classify yet), so close the gap before the last chunk: a client must end
                // with exactly the answer the turn produced.
                const std::string ft = stream_text(out.result.generated_text),
                                  fr = stream_text(out.result.reasoning_text);
                if (alive && (ft != sent_text || fr != sent_reasoning)) {
                    json delta = json::object();
                    json extra = nullptr;
                    if (starts_with(ft, sent_text) && starts_with(fr, sent_reasoning)) {
                        if (ft.size() > sent_text.size()) delta["content"] = ft.substr(sent_text.size());
                        if (fr.size() > sent_reasoning.size())
                            delta["reasoning_content"] = fr.substr(sent_reasoning.size());
                    } else {
                        delta["content"] = ft;
                        delta["reasoning_content"] = fr;
                        extra = json{{"reset", true}};
                    }
                    chunk(delta, nullptr, extra);
                }
                // A call is only known whole, once the turn has been parsed, so it travels in one
                // delta rather than argument by argument.
                if (!out.result.tool_calls.empty()) {
                    ensure_tool_call_ids(out.result.tool_calls, meta.id);
                    chunk(tool_calls_delta(out.result.tool_calls), nullptr, nullptr);
                }
                chunk(json::object(), finish_reason(out.result, out.n_predict),
                      json{{"summary", summary_json(out.result.summary, out.result.cancelled)}});
                if (cr.include_usage) write("data: " + dump(usage_chunk_json(meta, out.result.summary)) + "\n\n");
            }
            write("data: [DONE]\n\n");
            sink.done();
            return true;
        });
    });

    // The Anthropic Messages API, for Claude Code (anthropic_messages.h). The request names a model;
    // the loaded one answers, as on the chat endpoint.
    svr.Post("/v1/messages", [&](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        AnthropicRequest ar;
        std::string err;
        if (!parse_messages_request(body, app.store.config(), ar, err))
            return send_path_error(req.path, res, 400, err, "invalid_request");
        if (!app.host.ready())
            return send_path_error(req.path, res, 503, "no model is loaded: load one from the Models page",
                                   error_code::no_model);
        ChatResponseMeta meta;
        meta.id = "msg_" + std::to_string(++app.chat_seq);
        meta.created = (long long) std::time(nullptr);
        meta.model = app.host.model_name();
        agent_turn<AnthropicStream>(app, req, res, ar.turn, ar.stream, meta, message_json, AnthropicStream::ping());
    });

    // An estimate, not a count: the host has no tokenizer outside a turn (estimate_prompt_tokens).
    svr.Post("/v1/messages/count_tokens", [&](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        AnthropicRequest ar;
        std::string err;
        if (!parse_messages_request(body, app.store.config(), ar, err))
            return send_path_error(req.path, res, 400, err, "invalid_request");
        send_json(res, json{{"input_tokens", estimate_prompt_tokens(ar.turn)}});
    });

    // The OpenAI Responses API, for Codex CLI (openai_responses.h). Stateless: the whole
    // conversation comes in every request.
    svr.Post("/v1/responses", [&](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        ResponsesRequest rr;
        std::string err;
        if (!parse_responses_request(body, app.store.config(), rr, err))
            return send_path_error(req.path, res, 400, err, "invalid_request");
        if (!app.host.ready())
            return send_path_error(req.path, res, 503, "no model is loaded: load one from the Models page",
                                   error_code::no_model);
        ChatResponseMeta meta;
        meta.id = "resp_" + std::to_string(++app.chat_seq);
        meta.created = (long long) std::time(nullptr);
        meta.model = app.host.model_name();
        agent_turn<ResponsesStream>(app, req, res, rr.turn, rr.stream, meta, response_json,
                                    std::string(": waiting for a turn\n\n"));
    });

    // A decision: which of a list of choices the model would answer next, from one prefill and no
    // decode (docs/decide.md). Not streamed: there is nothing to stream, and the answer is one object.
    svr.Post("/api/decide", [&](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        DecideRequest dr;
        std::string err;
        if (!parse_decide_request(body, dr, err)) return send_error(res, 400, err);
        if (!app.host.ready())
            return send_error(res, 503, "no model is loaded: load one from the Models page", error_code::no_model);
        DecideOutcome out = app.host.decide(dr, [&req] { return !req.is_connection_closed(); });
        if (out.status != 200) return send_error(res, out.status, out.error, out.code);
        send_json(res, decide_json(out.result));
    });

    // A stream holds a worker for as long as it is open, so the bus caps how many there are and the
    // place is taken here, before the 200: past the cap the answer is a 503 the client can retry.
    // A client that closed its connection is seen within a liveness poll; one that keeps it open
    // but stops reading is let go by the ping, whose write times out (the server's write timeout)
    // once the socket's buffer is full.
    svr.Get("/api/events", [&](const httplib::Request & req, httplib::Response & res) {
        std::shared_ptr<EventBus::Queue> q = app.bus.subscribe(is_loopback_peer(req.remote_addr));
        if (!q)
            return send_error(res, 503,
                              "too many event streams are open (" + std::to_string(k_max_event_streams) +
                                  "): close another tab or app connected to this server",
                              error_code::streams_full);
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream",
            [&app, q](size_t, httplib::DataSink & sink) {
                std::string f =
                    EventBus::frame("state", app.host.info()) + EventBus::frame("config", app.config_object());
                bool ok = sink.write(f.data(), f.size());
                auto last_write = std::chrono::steady_clock::now();
                while (ok) {
                    std::string frame;
                    if (!EventBus::next(*q, frame, kEventLivenessPoll)) break;
                    if (frame.empty()) {
                        if (!sink.is_writable()) break;
                        if (std::chrono::steady_clock::now() - last_write < kEventPingInterval) continue;
                        frame = ": ping\n\n";
                    }
                    ok = sink.write(frame.data(), frame.size());
                    last_write = std::chrono::steady_clock::now();
                }
                sink.done();
                return true;
            },
            // Runs however the response ends, the provider never called included (a client that
            // left before its headers were sent), so the place is always given back.
            [&app, q](bool) { app.bus.unsubscribe(q); });
    });

    svr.Get("/api/models", [&](const httplib::Request &, httplib::Response & res) { send_json(res, app.lib.list()); });

    svr.Post("/api/models/download", [&](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        if (!body.contains("id") || !body["id"].is_string())
            return send_error(res, 400, "id must be a catalog id string", "invalid_request");
        const ModelLibrary::DownloadRefusal r = app.lib.start_download(body["id"].get<std::string>());
        if (r.status) return send_error(res, r.status, r.error);
        send_json(res, json{{"ok", true}}, 202);
    });

    svr.Delete(R"(/api/models/download/(.+))", [&](const httplib::Request & req, httplib::Response & res) {
        const bool cancelled = app.lib.cancel_download(req.matches[1].str());
        send_json(res, json{{"ok", cancelled}}, cancelled ? 200 : 404);
    });

    // A catalog id, never a path: the library deletes only the `.part` files that entry names, in
    // the models folder (download_policy.h).
    svr.Delete(R"(/api/models/partial/(.+))", [&](const httplib::Request & req, httplib::Response & res) {
        uint64_t freed = 0;
        const ModelLibrary::DownloadRefusal r = app.lib.delete_partial(req.matches[1].str(), freed);
        if (r.status) return send_error(res, r.status, r.error);
        send_json(res, json{{"ok", true}, {"freed", freed}});
    });

    // A model of the models folder, named as the listing names it (`folder/name`), never a path:
    // the library deletes the files the folder's own scan groups under that name (model_delete.h).
    // Only the person at this machine deletes files: a client on the network, key or not, may not
    // (local_only_routes, access.h, refuses it before routing).
    svr.Delete(R"(/api/models/local/(.+))", [&](const httplib::Request & req, httplib::Response & res) {
        // Under the load lock: a load cannot start on the file between the check and the delete.
        std::lock_guard<std::mutex> load(app.load_m);
        const std::string loaded = app.host.loaded_or_loading() ? app.host.model_path() : "";
        uint64_t freed = 0;
        const ModelLibrary::DownloadRefusal r = app.lib.delete_model(req.matches[1].str(), loaded, freed);
        if (r.status) return send_error(res, r.status, r.error, r.status == 409 ? "in_use" : "");
        send_json(res, json{{"ok", true}, {"freed", freed}});
    });

    // A direct link to a gguf becomes a catalog entry (url_entry.h), and its download is then the
    // catalog's, by id. The server fetches what a client named here, so only a local client may (local_only_routes).
    svr.Post("/api/models/url", [&](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        if (!body.contains("url") || !body["url"].is_string())
            return send_error(res, 400, "url must be a link to a gguf file", "invalid_request");
        UrlModel m;
        std::string err;
        if (!parse_model_url(body["url"].get<std::string>(), m, err)) return send_error(res, 400, err, "invalid_url");
        std::vector<uint64_t> sizes;
        bool timed_out = false;
        if (!fetch_url_sizes(*app.net, m, kUrlSizeDeadline, sizes, timed_out, err))
            return send_error(res, timed_out ? 504 : 502, err, "upstream");
        const CatalogEntry e = url_catalog_entry(m, sizes);
        err = app.lib.add_entry(e);
        if (!err.empty()) return send_error(res, 409, err);
        uint64_t total = 0;
        for (uint64_t n : sizes)
            total += n;
        send_json(res, json{{"id", e.id}, {"title", e.title}, {"quant", e.quant}, {"bytes", total}}, 201);
    });

    // ── a model from a Hugging Face repository (hf_repo.h, the helpers above register_routes) ──
    svr.Post("/api/hf/search", [&](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        if (!body.contains("query") || !body["query"].is_string())
            return send_error(res, 400, "query must be a string", "invalid_request");
        std::string query = body["query"].get<std::string>();
        while (!query.empty() && std::isspace((unsigned char) query.back()))
            query.pop_back();
        while (!query.empty() && std::isspace((unsigned char) query.front()))
            query.erase(query.begin());
        if (query.empty()) return send_error(res, 400, "query is empty", "invalid_request");
        std::string token;
        if (body.contains("token") && body["token"].is_string()) token = body["token"].get<std::string>();
        std::string text, err;
        if (!app.net->get(hf_search_url(app.opt.hf_endpoint, query, kHfSearchLimit), 0, 0, token, text, err))
            return send_error(res, 502, "could not search: " + err, "upstream");
        json hits;
        try {
            hits = json::parse(text);
        } catch (const std::exception &) {
            return send_error(res, 502, "the search answer is not JSON", "upstream");
        }
        std::vector<HfRepoHit> repos;
        if (!parse_hf_search(hits, repos, err)) return send_error(res, 502, err, "upstream");
        json out = json::array();
        for (const HfRepoHit & r : repos)
            out.push_back({{"id", r.id},
                           {"downloads", r.downloads},
                           {"likes", r.likes},
                           {"modified", r.modified},
                           {"gated", r.gated}});
        send_json(res, json{{"query", query}, {"repos", out}});
    });

    svr.Post("/api/hf/models", [&](const httplib::Request & req, httplib::Response & res) {
        HfRequest r;
        std::vector<HfModel> models;
        if (!read_hf_request(req, res, r, false) || !list_hf_models(app, res, r, models)) return;
        json out = json::array();
        for (const HfModel & m : models)
            out.push_back(hf_model_json(m));
        send_json(res, json{{"repo", r.ref.repo}, {"revision", r.ref.revision}, {"file", r.ref.file}, {"models", out}});
    });

    svr.Post("/api/hf/inspect", [&](const httplib::Request & req, httplib::Response & res) {
        HfRequest r;
        std::vector<HfModel> models;
        HfModel m;
        if (!read_hf_request(req, res, r, true) || !list_hf_models(app, res, r, models) ||
            !find_hf_model(res, r, models, m))
            return;
        json facts;
        std::string err;
        if (!inspect_remote_model(*app.net, app.opt.hf_endpoint, r.ref, m, r.token, app.scratch_dir,
                                  app.lib.models_dir(), app.loaded_session(), facts, err))
            return send_error(res, 502, "could not read the header of " + m.name + ": " + err, "upstream");
        send_json(res, facts);
    });

    svr.Post("/api/hf/add", [&](const httplib::Request & req, httplib::Response & res) {
        HfRequest r;
        std::vector<HfModel> models;
        HfModel m;
        if (!read_hf_request(req, res, r, true) || !list_hf_models(app, res, r, models) ||
            !find_hf_model(res, r, models, m))
            return;
        if (!m.complete())
            return send_error(res, 409, r.ref.repo + ": " + m.name + " is a split set with shards missing",
                              "incomplete");
        const CatalogEntry e = hf_catalog_entry(app.opt.hf_endpoint, r.ref, m);
        const std::string err = app.lib.add_entry(e);
        if (!err.empty()) return send_error(res, 409, err);
        send_json(res, json{{"id", e.id}, {"title", e.title}, {"quant", e.quant}, {"bytes", m.bytes}}, 201);
    });

    svr.Delete(R"(/api/hf/entry/(.+))", [&](const httplib::Request & req, httplib::Response & res) {
        const std::string id = req.matches[1].str();
        if (!is_hf_catalog_id(id) && !is_url_catalog_id(id))
            return send_error(res, 404, "not an added entry", "not_found");
        const bool removed = app.lib.remove_entry(id);
        send_json(res, json{{"ok", removed}}, removed ? 200 : 404);
    });

    svr.Get("/api/plan", [&](const httplib::Request & req, httplib::Response & res) {
        if (!planner_available())
            return send_json(res, json{{"available", false},
                                       {"reason", "this build does not include the automatic hardware planner"}});
        // With a model loaded, the answer is a plan this server holds for it (plan_view.h, held_plan):
        // measuring again now would count that model's memory as taken. `?measure=1` asks for a
        // fresh measurement anyway.
        if (!req.has_param("measure") && app.host.loaded_or_loading()) {
            PlanOutcome lp, last;
            {
                std::lock_guard<std::mutex> lk(app.plan_m);
                lp = app.load_plan;
                last = app.last_plan;
            }
            PlanRead r;
            r.model = app.store.config().model_path;
            r.want_cached = req.has_param("cached");
            if (lp.ok) {
                r.load_model = lp.body.value("model", "");
                r.load_seq = lp.seq;
            }
            if (last.ok) {
                r.last_model = last.body.value("model", "");
                r.last_seq = last.seq;
            }
            const HeldPlan held = held_plan(r);
            if (held != HeldPlan::None) {
                json body = held == HeldPlan::Runs ? lp.body : last.body;
                body[held == HeldPlan::Runs ? "from_last_load" : "cached"] = true;
                // What the loaded model has done since, beside the plan (compared to it by a UI only
                // when the model runs that plan).
                const json measured = app.host.last_turn();
                if (!measured.is_null()) body["measured"] = measured;
                return send_json(res, body);
            }
        }
        // `?cached=1`: the last plan this server holds, never a measurement. A screen opens on it, so
        // opening the Plan screen costs nothing on any machine and every UI shows the same thing.
        if (req.has_param("cached")) {
            PlanOutcome last;
            {
                std::lock_guard<std::mutex> lk(app.plan_m);
                last = app.last_plan.ok ? app.last_plan : app.load_plan;
            }
            if (!last.ok) return send_json(res, json{{"available", true}, {"none", true}});
            json body = last.body;
            body["cached"] = true;
            return send_json(res, body);
        }
        if (app.host.generating())
            return send_error(res, 409, "busy: a plan measures the machine, and a running generation would skew it");
        std::unique_lock<std::mutex> measuring(app.measure_m, std::try_to_lock);
        if (!measuring) return send_error(res, 409, "busy: a plan is already being measured");
        PlanOutcome p = app.measure_plan();
        measuring.unlock();
        if (!p.ok) return send_json(res, json{{"available", true}, {"error", p.error}});
        json body = p.body;
        if (app.host.loaded_or_loading())
            body["warning"] = "measured with a model loaded: its memory reads as taken, so this plan is sized for a "
                              "smaller machine than this one. Auto mode plans with the model unloaded.";
        {
            std::lock_guard<std::mutex> lk(app.plan_m);
            app.last_plan = std::move(p);
        }
        send_json(res, body);
    });

    svr.Post("/api/plan/apply", [&](const httplib::Request &, httplib::Response & res) {
        if (!planner_available())
            return send_error(res, 409, "this build does not include the automatic hardware planner");
        PlanOutcome p;
        {
            std::lock_guard<std::mutex> lk(app.plan_m);
            p = app.last_plan;
        }
        if (!p.ok || p.body.value("model", "") != app.store.config().model_path)
            return send_error(res, 409, "compute a plan for the current model first (GET /api/plan)");
        app.store.apply_plan(p.values, p.decisions);
        send_json(res, app.config_object());
        app.publish_config();
    });

    // ── backend packs and updates (update_service.h; local clients only, access.h) ──
    const auto reply = [](httplib::Response & res, const UpdateService::Reply & r) {
        send_json(res, r.body, r.status);
    };
    svr.Get("/api/backends", [&, reply](const httplib::Request & req, httplib::Response & res) {
        reply(res, app.updates->backends(req.get_param_value("refresh") == "1"));
    });
    svr.Post(R"(/api/backends/([^/]+)/install)", [&, reply](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        const bool force = body.contains("force") && body["force"].is_boolean() && body["force"].get<bool>();
        reply(res, app.updates->install_pack(req.matches[1].str(), force));
    });
    svr.Get("/api/update",
            [&, reply](const httplib::Request &, httplib::Response & res) { reply(res, app.updates->update()); });
    svr.Post("/api/update/download", [&, reply](const httplib::Request &, httplib::Response & res) {
        reply(res, app.updates->download_update());
    });
}

// Every rule about who may reach the API lives in access.h; here the request is read into the
// facts those rules take, ahead of routing, so that no handler can forget one.
void register_guards(httplib::Server & svr, const Options & o) {
    AccessPolicy policy;
    policy.loopback = is_loopback_host(o.host);
    policy.api_key = o.api_key;
    policy.origins = {"http://127.0.0.1:" + std::to_string(o.port), "http://localhost:" + std::to_string(o.port),
                      "http://[::1]:" + std::to_string(o.port)};
    policy.origins.insert(o.allow_origins.begin(), o.allow_origins.end());
    svr.set_pre_routing_handler([policy](const httplib::Request & req, httplib::Response & res) {
        RequestFacts f;
        f.method = req.method;
        f.path = req.path;
        f.host = req.get_header_value("Host");
        f.has_origin = req.has_header("Origin");
        f.origin = req.get_header_value("Origin");
        f.sec_fetch_site = req.get_header_value("Sec-Fetch-Site");
        f.content_type = req.get_header_value("Content-Type");
        f.authorization = req.get_header_value("Authorization");
        f.x_api_key = req.get_header_value("X-Api-Key");
        f.remote_addr = req.remote_addr;
        AccessDecision d = check_access(policy, f);
        // What reaches this computer's files, processes or keys answers to this computer only, key
        // or no key (docs/agent.md). A route that depends on its conversation checks in its handler.
        if (d.allowed() && agent_route_locality(req.path) == Locality::Always && !is_loopback_peer(req.remote_addr))
            d = {403, "only a client on this computer may do this: it reaches this computer's files, processes or keys",
                 k_local_only_code};
        if (d.allowed()) return httplib::Server::HandlerResponse::Unhandled;
        if (d.status == 401) res.set_header("WWW-Authenticate", "Bearer realm=\"bmoe-server\"");
        send_path_error(req.path, res, d.status, d.error, d.code);
        return httplib::Server::HandlerResponse::Handled;
    });
    // On every response, the refusals above and the UI's files alike.
    svr.set_post_routing_handler([](const httplib::Request & req, httplib::Response & res) {
        const bool preview = is_preview_path(req.path) && res.status == 200;
        for (const auto & h : preview ? preview_headers() : security_headers())
            if (!res.has_header(h.first)) res.set_header(h.first, h.second);
    });
    // A handler that reads a field of the wrong type out of a request throws; that is the client's
    // mistake (400), not the server's (500). Anything else is a fault here, said as one.
    svr.set_exception_handler([](const httplib::Request & req, httplib::Response & res, std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const json::exception & e) {
            send_path_error(req.path, res, 400, std::string("malformed request: ") + e.what(), "invalid_request");
        } catch (const std::exception & e) {
            send_path_error(req.path, res, 500, std::string("internal error: ") + e.what(), "failed");
        } catch (...) {
            send_path_error(req.path, res, 500, "internal error", "failed");
        }
    });
}

} // namespace

namespace bmoe::server {

int run(int argc, char ** argv) {
    return run(argc, argv, Embedding{});
}

int run(int argc, char ** argv, const Embedding & embedding) {
    Options o;
    const int rc = parse_args(argc, argv, o);
    if (rc >= 0) return rc;
    const std::string data_dir = o.data_dir.empty() ? platform::default_data_dir() : o.data_dir;
    // What a backend reads from its environment, set before any backend is loaded.
    platform::prepare_compute_backends(data_dir);
    // Every backend beside the executable, then the packs installed into the data folder (a pack
    // replaced while in use is moved into place first), before anything asks ggml about devices.
    const std::string backends_root = (fs::u8path(data_dir) / "backends").u8string();
    for (const std::string & w : finish_pending_packs(backends_root))
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    const std::vector<std::string> pack_dirs = backend_pack_dirs(backends_root);
    platform::prepare_backend_modules(extra_backend_modules(platform::executable_dir(), pack_dirs));
    register_backends(pack_dirs);
    // The child of an isolated device probe: one device, its facts on one line, nothing else. If
    // the driver ends this process, or it never returns, that is the parent's answer about the device.
    if (!o.probe_device.empty()) {
        const auto model = o.launch.find("model");
        if (model == o.launch.end() || model->second.empty()) {
            std::fprintf(stderr, "bmoe-server: --probe-device needs -m MODEL\n");
            return 2;
        }
        return run_device_probe_child(model->second, o.probe_device);
    }
    const std::string refusal = launch_refusal(o.host, o.api_key, o.no_api_key);
    if (!refusal.empty()) {
        std::fprintf(stderr, "bmoe-server: %s\n", refusal.c_str());
        return 2;
    }

    const fs::path exe = fs::u8path(platform::executable_dir());
    // Absolute from here on: the library lists models by these paths and the confinement resolves a
    // relative request against the first, so a relative one typed at launch would count twice.
    const auto absolute = [](const std::string & p) {
        std::error_code ec;
        const fs::path a = fs::absolute(fs::u8path(p), ec);
        return ec ? p : a.lexically_normal().u8string();
    };
    const std::string models_dir =
        absolute(o.models_dir.empty() ? (fs::u8path(data_dir) / "models").u8string() : o.models_dir);
    for (std::string & d : o.scan_dirs)
        d = absolute(d);
    const std::string ui_dir = o.ui_dir.empty()
                                   ? first_existing({exe / "ui" / "index.html"}).empty() ? "" : (exe / "ui").u8string()
                                   : o.ui_dir;
    const std::string catalog = o.catalog.empty() ? (exe / "catalog" / "models.json").u8string() : o.catalog;
    const std::string settings = (fs::u8path(data_dir) / "settings.json").u8string();
    const std::string added = (fs::u8path(data_dir) / "added-models.json").u8string();
    const std::string scratch = (fs::u8path(data_dir) / "tmp").u8string();

    const std::string routing_dir = (fs::u8path(data_dir) / "routing").u8string();
    App app(o, settings, models_dir, catalog, added, scratch, routing_dir, embedding.fetch);
    for (const std::string & w : app.store.load())
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    for (const std::string & w : app.lib.warnings())
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    app.store.set_launch_values(o.launch);
    app.host.set_host_info(platform::host_info);
    app.host.set_planner_available(planner_available());
    app.host.set_serves_network(!is_loopback_host(o.host));
    {
        UpdateConfig uc;
        uc.data_dir = data_dir;
        uc.feed_url = o.feed_url;
        uc.public_key = kFeedPublicKey;
        uc.target = platform::build_target();
        uc.version = version();
        uc.registered = backend_inventory().names;
        uc.library_loads = platform::library_loads;
        app.updates = std::make_unique<UpdateService>(app.bus, *app.net, uc);
    }

    app.agent_store = std::make_unique<agent::AgentStore>(data_dir);
    for (const std::string & w : app.agent_store->load())
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    {
        // The clients a person can connect, beside the model catalog; a server without it lists none.
        const fs::path clients = fs::u8path(catalog).parent_path() / "clients.json";
        std::ifstream in(clients, std::ios::binary);
        if (in) {
            try {
                app.agent_routes.clients = json::parse(in);
            } catch (const std::exception & e) {
                std::fprintf(stderr, "warning: %s does not parse: %s\n", clients.u8string().c_str(), e.what());
            }
        }
    }
    if (!o.chat_only && platform::can_run_commands()) {
        // The MCP servers (docs/agent.md): an explicit file must exist; the default one may not.
        const std::string path = o.mcp_config.empty() ? (fs::u8path(data_dir) / "mcp.json").u8string() : o.mcp_config;
        std::error_code ec;
        if (fs::exists(fs::u8path(path), ec)) {
            std::ifstream in(fs::u8path(path), std::ios::binary);
            std::vector<std::string> warnings;
            std::vector<McpServerConfig> servers;
            try {
                servers = parse_mcp_config(json::parse(in), warnings);
            } catch (const std::exception & e) {
                warnings.push_back(path + " does not parse: " + e.what());
            }
            for (const std::string & w : warnings)
                std::fprintf(stderr, "warning: %s\n", w.c_str());
            if (!servers.empty()) app.mcp = std::make_unique<McpHub>(std::move(servers));
        } else if (!o.mcp_config.empty()) {
            std::fprintf(stderr, "warning: --mcp-servers-config %s does not exist\n", path.c_str());
        }
    }
    {
        agent::AgentDeps deps;
        deps.bus = &app.bus;
        deps.store = app.agent_store.get();
        deps.local_chat = [&app](const ChatRequest & req, const std::function<bool(const TokenMetrics &)> & on_token,
                                 const std::function<bool()> & still_wanted) {
            return app.host.chat(req, on_token, still_wanted);
        };
        deps.config = [&app] { return app.store.config(); };
        deps.net = app.net.get();
        deps.mcp = app.mcp.get();
        deps.bundled_skills_dir = o.skills_dir.empty() ? (exe / "skills").u8string() : o.skills_dir;
        deps.user_skills_dir = (fs::u8path(data_dir) / "skills").u8string();
        deps.tools = !o.chat_only;
        deps.can_run_commands = platform::can_run_commands();
        deps.os_name = platform::os_name();
        deps.external_agents = acp_agents(app.agent_routes.clients);
        app.agent = std::make_unique<agent::Agent>(std::move(deps));
    }
    app.agent_routes.agent = app.agent.get();
    app.agent_routes.store = app.agent_store.get();
    app.agent_routes.host = &app.host;
    app.agent_routes.bus = &app.bus;
    app.agent_routes.user_skills_dir = (fs::u8path(data_dir) / "skills").u8string();
    app.agent_routes.has_key = !o.api_key.empty();
    app.agent_routes.home = platform::home_dir();

    httplib::Server svr;
    // A worker is held for a request's whole life, and a connection past the queue is closed at
    // once instead of waiting with no bound behind workers that may not come free.
    svr.new_task_queue = [] { return new httplib::ThreadPool(kWorkerThreads, 0, kMaxQueuedRequests); };
    svr.set_socket_options(set_listener_options);
    // A request body here is a chat history or a handful of settings. The library's default would
    // let every worker buffer a hundred times that.
    svr.set_payload_max_length(8ull << 20);
    register_guards(svr, o);
    register_routes(svr, app);
    register_agent_routes(svr, app.agent_routes);
    if (!ui_dir.empty()) {
        // no-cache means "revalidate", not "never store": the browser keeps the files and asks with
        // the ETag each time, so a rebuilt UI is picked up on the next load instead of whenever the
        // browser's heuristic decides the old index.html has aged out.
        svr.set_mount_point("/", ui_dir, httplib::Headers{{"Cache-Control", "no-cache"}});
    } else {
        svr.Get("/", [](const httplib::Request &, httplib::Response & res) {
            res.set_content("<!doctype html><title>bmoe-server</title><p>The web UI is not built. Run "
                            "<code>npm ci &amp;&amp; npm run build</code> in <code>ui/</code> and rebuild, or pass "
                            "<code>--ui-dir</code>. The API is up: see docs/server-api.md.</p>",
                            "text/html");
        });
    }

    if (!svr.bind_to_port(o.host, o.port)) {
        std::fprintf(stderr, "bmoe-server: cannot listen on %s:%d (in use?)\n", o.host.c_str(), o.port);
        return 1;
    }
    const std::string url = browse_url(o.host, o.port);
    {
        std::string origin = url;
        if (!origin.empty() && origin.back() == '/') origin.pop_back();
        app.agent_routes.origin = origin;
    }
    if (!o.chat_only) app.agent->start_scheduler();
    std::printf("bmoe-server %s listening on %s\n", version(), url.c_str());
    std::printf("  data: %s\n  models: %s\n  ui: %s\n", data_dir.c_str(), models_dir.c_str(),
                ui_dir.empty() ? "(not built)" : ui_dir.c_str());
    if (!o.api_key.empty()) std::printf("  API key: required (Authorization: Bearer ...)\n");
    if (!is_loopback_host(o.host))
        std::printf(
            o.api_key.empty()
                ? "  WARNING: bound to %s with no API key: anyone who can reach this address can use the engine.\n"
                : "  bound to %s: reachable from the network, with the key. Prompts and answers travel in clear.\n",
            o.host.c_str());
    std::fflush(stdout);

    if (o.launch.count("model")) {
        const std::string err = app.load_current();
        if (!err.empty()) std::fprintf(stderr, "warning: not loading the model at start: %s\n", err.c_str());
    }
    if (o.open_browser || (argc == 1 && platform::started_by_double_click())) platform::open_url(url);

    // A stop ends what keeps a worker busy first (a generation, every event stream), since the
    // server's stop waits for its workers.
    const auto stop = [&svr, &app] {
        app.host.cancel();
        app.agent->shutdown();
        app.updates->cancel();
        app.bus.close_all();
        svr.stop();
    };
    if (embedding.listening) embedding.listening(stop);
    // The app that started this server is looked for every second; once it is gone, so is the
    // server, whatever ended the app.
    std::atomic<bool> listening_done{false};
    std::thread parent_watch;
    if (o.parent_pid > 0)
        parent_watch = std::thread([&] {
            while (!listening_done) {
                if (!platform::process_alive(o.parent_pid)) {
                    std::fprintf(stderr, "bmoe-server: the process that started it (%ld) ended; stopping\n",
                                 o.parent_pid);
                    stop();
                    return;
                }
                std::this_thread::sleep_for(kParentPollInterval);
            }
        });
    svr.listen_after_bind();
    listening_done = true;
    if (parent_watch.joinable()) parent_watch.join();
    if (embedding.listening) embedding.listening(nullptr);
    // Every run and the scheduler end here, before the host they ask goes away.
    app.agent->shutdown();
    app.bus.close_all();
    return 0;
}

} // namespace bmoe::server
