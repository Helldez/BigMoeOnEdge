// The HTTP face of bmoe-server, and its composition root (server.h).
//
// Composes the engine (one EngineHost), the configuration (SettingsStore), the model files
// (ModelLibrary) and the event stream (EventBus) behind the HTTP API in docs/server-api.md, and
// serves the UI's static files. Like bmoe-cli it is a front-end: every engine tunable arrives
// through the parameter table, and nothing here knows a parameter by more than its key. Who may
// reach the API is decided in access.h; this file only reads the request and answers.
#include "server.h"

#include "access.h"
#include "decide_protocol.h"
#include "engine_host.h"
#include "openai_chat.h"
#include "event_bus.h"
#include "json_util.h"
#include "model_library.h"
#include "planner_adapter.h"
#include "platform.h"
#include "settings_store.h"

#include "bmoe/config.h"
#include "bmoe/params.h"
#include "bmoe/recipe.h"
#include "bmoe/version.h"

#include <cpp-httplib/httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace bmoe;
using namespace bmoe::server;

namespace {

constexpr int kPortMax = 65535;

struct Options {
    std::string host = kDefaultHost;
    int port = kDefaultPort;
    std::string ui_dir, models_dir, data_dir, catalog;
    std::vector<std::string> scan_dirs; // more directories whose models are listed, never written
    std::vector<std::string> allow_origins;
    std::string api_key;
    std::string api_key_file; // --api-key-file: the key, read at start, never on the command line
    bool no_api_key = false;  // listen beyond loopback with no key, on purpose
    bool models_only = false; // every client, local ones too, names models in the models folder only
    bool open_browser = false;
    bool isolated_probes = true;               // the planner asks each device in a child process first
    std::string probe_device;                  // internal: this process IS such a child; probe this device and exit
    std::map<std::string, std::string> launch; // engine parameters given on the command line
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
#if defined(BMOE_HAVE_EXPERT_READY_HOOK)
    c.moe.overlap = true; // byte-identical and the measured best on every device; needs the hook
#endif
    return c;
}

void print_usage(const char * argv0) {
    std::printf("usage: %s [options] [engine parameters]\n"
                "\n"
                "  --host ADDR          address to bind (default %s; anything else exposes the engine)\n"
                "  --port N             port (default %d)\n"
                "  --open               open the UI in the default browser once listening\n"
                "  --models-dir PATH    where models are listed and downloaded (default: <data dir>/models)\n"
                "  --scan-dir PATH      also list the models in PATH, loaded in place (repeatable)\n"
                "  --data-dir PATH      settings and models (default: the per-user application data folder)\n"
                "  --ui-dir PATH        the built web UI (default: ui/ beside this binary)\n"
                "  --catalog PATH       the model catalog (default: catalog/models.json beside this binary)\n"
                "  --allow-origin URL   also accept requests from this browser origin (a UI dev server)\n"
                "  --api-key KEY        require this key on the API (Authorization: Bearer KEY); needed\n"
                "                       with a --host beyond loopback. Other local users can read a\n"
                "                       command line: prefer --api-key-file\n"
                "  --api-key-file PATH  the same, with the key read from PATH (trailing newline ignored)\n"
                "  --no-api-key         listen beyond loopback with no key, on a network you trust\n"
                "  --models-only        local clients too may only open models from the folders models\n"
                "                       are listed from (clients on the network always are)\n"
                "  --no-isolated-probes the planner probes compute devices in this process instead of a child each\n"
                "  -h, --help           this text\n"
                "      --version        print the engine version\n"
                "\n"
                "Engine parameters are the bmoe-cli flags (bmoe-cli --help lists them). Given here, they\n"
                "hold for this run and outrank saved settings; -m loads that model at start.\n",
                argv0, kDefaultHost, kDefaultPort);
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
        else if (a == "--no-isolated-probes")
            o.isolated_probes = false;
        else if (a == "--probe-device")
            o.probe_device = next();
        else if (a == "-h" || a == "--help") {
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
        send_error(res, 400, std::string("invalid JSON body: ") + e.what(), "invalid_request");
        return false;
    }
    if (!out.is_object()) {
        send_error(res, 400, "the request body must be a JSON object", "invalid_request");
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

    App(Options o, std::string settings_path, std::string models_dir, std::string catalog)
        : opt(std::move(o)), store(server_defaults(), std::move(settings_path)), host(bus),
          lib(std::move(models_dir), opt.scan_dirs, std::move(catalog), bus) {}

    json config_object(const json * rejected = nullptr) {
        const RunConfig cfg = store.config();
        const ValidationResult v = validate(cfg);
        json o = {{"values", config_values(cfg)},
                  {"user_keys", store.operator_keys()},
                  {"args", to_args(cfg)},
                  {"valid", v.ok},
                  {"error", v.error},
                  {"reload_required", host.reload_required(cfg)},
                  {"plan", store.plan_decisions()},
                  {"auto_plan", auto_plan()}};
        if (rejected) o["rejected"] = *rejected;
        return o;
    }

    void publish_config() { bus.publish("config", config_object()); }

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
            PlanOutcome p = make_plan(store.planning_config(), store.operator_keys(), opt.isolated_probes);
            measuring.unlock();
            if (p.ok) {
                store.apply_plan(p.values, p.decisions);
                std::lock_guard<std::mutex> lk(plan_m);
                load_plan = p;
                last_plan = std::move(p);
            }
            publish_config();
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
        const std::string arch = lib.probe_arch(requested.model_path);
        if (effective.moe.enabled && !pinned && !arch.empty() && !find_moe_recipe(arch.c_str()))
            effective.moe.enabled = false;
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
    }
};

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
        if (!parse_chat_request(body, app.store.config(), cr, err)) return send_error(res, 400, err);
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
            bool alive = true;
            auto write = [&](const std::string & frame) {
                if (alive && !sink.write(frame.data(), frame.size())) {
                    alive = false;
                    app.host.cancel(); // the client went away: stop spending the machine on it
                }
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
            bool first = true;
            ChatOutcome out = app.host.chat(
                cr.turn,
                [&](const TokenMetrics & m) {
                    if (!alive) return;
                    json delta = json::object();
                    json extra = nullptr;
                    if (first) delta["role"] = "assistant";
                    if (starts_with(m.text, sent_text) && starts_with(m.reasoning, sent_reasoning)) {
                        const std::string dt = m.text.substr(sent_text.size());
                        const std::string dr = m.reasoning.substr(sent_reasoning.size());
                        if (dt.empty() && dr.empty() && !first) return;
                        if (!dt.empty()) delta["content"] = dt;
                        if (!dr.empty()) delta["reasoning_content"] = dr;
                    } else {
                        delta["content"] = m.text;
                        delta["reasoning_content"] = m.reasoning;
                        extra = json{{"reset", true}};
                    }
                    first = false;
                    sent_text = m.text;
                    sent_reasoning = m.reasoning;
                    chunk(delta, nullptr, extra);
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
                const std::string ft = out.result.generated_text, fr = out.result.reasoning_text;
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

    svr.Get("/api/events", [&](const httplib::Request &, httplib::Response & res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider("text/event-stream", [&app](size_t, httplib::DataSink & sink) {
            auto q = app.bus.subscribe();
            std::string f = EventBus::frame("state", app.host.info()) + EventBus::frame("config", app.config_object());
            bool ok = sink.write(f.data(), f.size());
            while (ok) {
                std::string frame;
                if (!EventBus::next(*q, frame, std::chrono::seconds(15))) break;
                if (frame.empty()) frame = ": ping\n\n";
                ok = sink.write(frame.data(), frame.size());
            }
            app.bus.unsubscribe(q);
            sink.done();
            return true;
        });
    });

    svr.Get("/api/models", [&](const httplib::Request &, httplib::Response & res) { send_json(res, app.lib.list()); });

    svr.Post("/api/models/download", [&](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        if (!body.contains("id") || !body["id"].is_string())
            return send_error(res, 400, "id must be a catalog id string", "invalid_request");
        const std::string err = app.lib.start_download(body["id"].get<std::string>());
        if (!err.empty()) return send_error(res, 404, err);
        send_json(res, json{{"ok", true}}, 202);
    });

    svr.Delete(R"(/api/models/download/(.+))", [&](const httplib::Request & req, httplib::Response & res) {
        const bool cancelled = app.lib.cancel_download(req.matches[1].str());
        send_json(res, json{{"ok", cancelled}}, cancelled ? 200 : 404);
    });

    svr.Get("/api/plan", [&](const httplib::Request & req, httplib::Response & res) {
        if (!planner_available())
            return send_json(res, json{{"available", false},
                                       {"reason", "this build does not include the automatic hardware planner"}});
        // With a model loaded, the answer is the plan it runs with: measuring again now would count
        // that model's memory as taken. `?measure=1` asks for a fresh measurement anyway.
        if (!req.has_param("measure") && app.host.loaded_or_loading()) {
            PlanOutcome lp;
            {
                std::lock_guard<std::mutex> lk(app.plan_m);
                lp = app.load_plan;
            }
            if (lp.ok && lp.body.value("model", "") == app.store.config().model_path) {
                json body = lp.body;
                body["from_last_load"] = true;
                return send_json(res, body);
            }
        }
        if (app.host.generating())
            return send_error(res, 409, "busy: a plan measures the machine, and a running generation would skew it");
        std::unique_lock<std::mutex> measuring(app.measure_m, std::try_to_lock);
        if (!measuring) return send_error(res, 409, "busy: a plan is already being measured");
        PlanOutcome p = make_plan(app.store.planning_config(), app.store.operator_keys(), app.opt.isolated_probes);
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
        const AccessDecision d = check_access(policy, f);
        if (d.allowed()) return httplib::Server::HandlerResponse::Unhandled;
        if (d.status == 401) res.set_header("WWW-Authenticate", "Bearer realm=\"bmoe-server\"");
        send_error(res, d.status, d.error);
        return httplib::Server::HandlerResponse::Handled;
    });
    // On every response, the refusals above and the UI's files alike.
    svr.set_post_routing_handler([](const httplib::Request &, httplib::Response & res) {
        for (const auto & h : security_headers())
            if (!res.has_header(h.first)) res.set_header(h.first, h.second);
    });
    // A handler that reads a field of the wrong type out of a request throws; that is the client's
    // mistake (400), not the server's (500). Anything else is a fault here, said as one.
    svr.set_exception_handler([](const httplib::Request &, httplib::Response & res, std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const json::exception & e) {
            send_error(res, 400, std::string("malformed request: ") + e.what(), "invalid_request");
        } catch (const std::exception & e) {
            send_error(res, 500, std::string("internal error: ") + e.what(), "failed");
        } catch (...) {
            send_error(res, 500, "internal error", "failed");
        }
    });
}

} // namespace

namespace bmoe::server {

int run(int argc, char ** argv) {
    platform::prepare_compute_backends();
    Options o;
    const int rc = parse_args(argc, argv, o);
    if (rc >= 0) return rc;
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
    const std::string data_dir = o.data_dir.empty() ? platform::default_data_dir() : o.data_dir;
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

    App app(o, settings, models_dir, catalog);
    for (const std::string & w : app.store.load())
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    for (const std::string & w : app.lib.warnings())
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    app.store.set_launch_values(o.launch);
    app.host.set_host_info(platform::host_info);
    app.host.set_planner_available(planner_available());

    httplib::Server svr;
    // Every open UI holds one /api/events stream for its lifetime, and a chat holds another: the
    // pool must be wider than the handful of tabs a person keeps open.
    svr.new_task_queue = [] { return new httplib::ThreadPool(32); };
    // A request body here is a chat history or a handful of settings. The library's default would
    // let every worker buffer a hundred times that.
    svr.set_payload_max_length(8ull << 20);
    register_guards(svr, o);
    register_routes(svr, app);
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
    const std::string url =
        "http://" + (o.host == "0.0.0.0" ? std::string("127.0.0.1") : o.host) + ":" + std::to_string(o.port) + "/";
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

    svr.listen_after_bind();
    app.bus.close_all();
    return 0;
}

} // namespace bmoe::server
