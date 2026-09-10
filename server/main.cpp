// bmoe-server: the engine behind a local web UI.
//
// Composes the engine (one EngineHost), the configuration (SettingsStore), the model files
// (ModelLibrary) and the event stream (EventBus) behind the HTTP API in docs/server-api.md, and
// serves the UI's static files. Like bmoe-cli it is a front-end: every engine tunable arrives
// through the parameter table, and nothing here knows a parameter by more than its key.
#include "engine_host.h"
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

struct Options {
    std::string host = "127.0.0.1";
    int port = 8765;
    std::string ui_dir, models_dir, data_dir, catalog;
    std::vector<std::string> allow_origins;
    bool open_browser = false;
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
    c.n_ctx = 4096;
    c.n_predict = 1024;
#if defined(BMOE_HAVE_EXPERT_READY_HOOK)
    c.moe.overlap = true; // byte-identical and the measured best on every device; needs the hook
#endif
    return c;
}

void print_usage(const char * argv0) {
    std::printf("usage: %s [options] [engine parameters]\n"
                "\n"
                "  --host ADDR          address to bind (default 127.0.0.1; anything else exposes the engine)\n"
                "  --port N             port (default 8765)\n"
                "  --open               open the UI in the default browser once listening\n"
                "  --models-dir PATH    where models are listed and downloaded (default: <data dir>/models)\n"
                "  --data-dir PATH      settings and models (default: the per-user application data folder)\n"
                "  --ui-dir PATH        the built web UI (default: ui/ beside this binary)\n"
                "  --catalog PATH       the model catalog (default: catalog/models.json beside this binary)\n"
                "  --allow-origin URL   also accept requests from this browser origin (a UI dev server)\n"
                "  -h, --help           this text\n"
                "      --version        print the engine version\n"
                "\n"
                "Engine parameters are the bmoe-cli flags (bmoe-cli --help lists them). Given here, they\n"
                "hold for this run and outrank saved settings; -m loads that model at start.\n",
                argv0);
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
        else if (a == "--port")
            o.port = std::atoi(next().c_str());
        else if (a == "--open")
            o.open_browser = true;
        else if (a == "--models-dir")
            o.models_dir = next();
        else if (a == "--data-dir")
            o.data_dir = next();
        else if (a == "--ui-dir")
            o.ui_dir = next();
        else if (a == "--catalog")
            o.catalog = next();
        else if (a == "--allow-origin")
            o.allow_origins.push_back(next());
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
    if (o.port <= 0 || o.port > 65535) {
        std::fprintf(stderr, "bmoe-server: --port must be in 1..65535\n");
        return 2;
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

bool is_loopback(const std::string & host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

void send_json(httplib::Response & res, const json & j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
}

void send_error(httplib::Response & res, int status, const std::string & msg) {
    send_json(res, json{{"error", msg}}, status);
}

bool parse_body(const httplib::Request & req, httplib::Response & res, json & out) {
    if (req.body.empty()) {
        out = json::object();
        return true;
    }
    try {
        out = json::parse(req.body);
        return true;
    } catch (const std::exception & e) {
        send_error(res, 400, std::string("invalid JSON body: ") + e.what());
        return false;
    }
}

bool starts_with(const std::string & s, const std::string & prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// OpenAI message content: a string, or an array of parts of which the text ones are kept.
bool message_text(const json & content, std::string & out) {
    if (content.is_string()) {
        out = content.get<std::string>();
        return true;
    }
    if (!content.is_array()) return false;
    out.clear();
    for (const json & part : content) {
        if (!part.is_object() || part.value("type", "") != "text") return false;
        out += part.value("text", "");
    }
    return true;
}

bool parse_chat_request(const json & body, const RunConfig & cfg, ChatRequest & cr, std::string & err) {
    const json msgs = body.value("messages", json());
    if (!msgs.is_array() || msgs.empty()) {
        err = "messages must be a non-empty array";
        return false;
    }
    for (const json & m : msgs) {
        std::string role = m.is_object() ? m.value("role", "") : "";
        if (role == "developer") role = "system"; // the newer OpenAI name for the same thing
        if (role != "system" && role != "user" && role != "assistant") {
            err = "unsupported message role '" + role + "' (system, user and assistant are)";
            return false;
        }
        std::string text;
        if (!message_text(m.value("content", json("")), text)) {
            err = "message content must be a string or an array of text parts";
            return false;
        }
        cr.messages.push_back({role, text});
    }
    cr.n_predict = cfg.n_predict;
    for (const char * k : {"max_completion_tokens", "max_tokens"}) {
        if (body.contains(k) && body[k].is_number_integer() && body[k].get<int>() > 0) {
            cr.n_predict = body[k].get<int>();
            break;
        }
    }
    cr.think = cfg.think;
    if (body.contains("think") && body["think"].is_boolean())
        cr.think = body["think"].get<bool>();
    else if (body.contains("chat_template_kwargs") && body["chat_template_kwargs"].is_object()) {
        const json & kw = body["chat_template_kwargs"];
        if (kw.contains("enable_thinking") && kw["enable_thinking"].is_boolean())
            cr.think = kw["enable_thinking"].get<bool>();
    }
    return true;
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
    PlanOutcome last_plan;

    App(Options o, std::string settings_path, std::string models_dir, std::string catalog)
        : opt(std::move(o)), store(server_defaults(), std::move(settings_path)), host(bus),
          lib(std::move(models_dir), std::move(catalog), bus) {}

    json config_object(const json * rejected = nullptr) {
        const RunConfig cfg = store.config();
        const ValidationResult v = validate(cfg);
        json o = {{"values", config_values(cfg)},
                  {"user_keys", store.operator_keys()},
                  {"args", to_args(cfg)},
                  {"valid", v.ok},
                  {"error", v.error},
                  {"reload_required", host.reload_required(cfg)},
                  {"plan", store.plan_decisions()}};
        if (rejected) o["rejected"] = *rejected;
        return o;
    }

    void publish_config() { bus.publish("config", config_object()); }

    // Load the configured model. A model this build does not stream (a dense one) cannot open with
    // streaming on, so streaming follows the model unless the user set it themselves.
    std::string load_current() {
        const RunConfig requested = store.config();
        if (requested.model_path.empty()) return "no model selected";
        RunConfig effective = requested;
        const std::vector<std::string> ops = store.operator_keys();
        const bool pinned = std::find(ops.begin(), ops.end(), "moe-stream") != ops.end();
        const std::string arch = lib.probe_arch(requested.model_path);
        if (effective.moe.enabled && !pinned && !arch.empty() && !find_moe_recipe(arch.c_str())) {
            effective.moe.enabled = false;
            effective.moe.overlap = false;
            effective.moe.io_two_wave = false;
        }
        return host.load(effective, requested);
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
        std::vector<std::string> reset_keys;
        for (const json & k : reset)
            if (k.is_string()) reset_keys.push_back(k.get<std::string>());
        const json rejected = app.store.apply(values, reset_keys);
        send_json(res, app.config_object(&rejected));
        app.publish_config();
    });

    svr.Post("/api/session/load", [&](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!parse_body(req, res, body)) return;
        if (body.contains("model")) {
            if (!body["model"].is_string()) return send_error(res, 400, "model must be a path string");
            app.store.apply(json{{"model", body["model"]}}, {});
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
        ChatRequest cr;
        std::string err;
        if (!parse_chat_request(body, app.store.config(), cr, err)) return send_error(res, 400, err);
        if (!app.host.ready()) return send_error(res, 503, "no model is loaded: load one from the Models page");
        if (app.host.generating()) return send_error(res, 409, "busy: a generation is already running");

        const std::string id = "chatcmpl-" + std::to_string(++app.chat_seq);
        const long long created = (long long) std::time(nullptr);
        const std::string model = app.host.model_name();

        if (!body.value("stream", false)) {
            ChatOutcome out = app.host.chat(cr, nullptr);
            if (out.status != 200) return send_error(res, out.status, out.error);
            json msg = {{"role", "assistant"}, {"content", out.result.generated_text}};
            if (!out.result.reasoning_text.empty()) msg["reasoning_content"] = out.result.reasoning_text;
            const RunSummary & s = out.result.summary;
            return send_json(
                res,
                json{{"id", id},
                     {"object", "chat.completion"},
                     {"created", created},
                     {"model", model},
                     {"choices", json::array({{{"index", 0}, {"message", msg}, {"finish_reason", out.finish_reason}}})},
                     {"usage",
                      {{"prompt_tokens", s.n_prompt},
                       {"completion_tokens", s.n_generated},
                       {"total_tokens", s.n_prompt + s.n_generated}}},
                     {"bmoe",
                      {{"summary", summary_json(s, out.result.cancelled)}, {"history_dropped", out.history_dropped}}}});
        }

        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream", [&app, cr, id, created, model](size_t, httplib::DataSink & sink) {
                bool alive = true;
                auto write = [&](const std::string & frame) {
                    if (alive && !sink.write(frame.data(), frame.size())) {
                        alive = false;
                        app.host.cancel(); // the client went away: stop spending the machine on it
                    }
                };
                auto chunk = [&](const json & delta, const json & finish, const json & extra) {
                    json c = {{"id", id},
                              {"object", "chat.completion.chunk"},
                              {"created", created},
                              {"model", model},
                              {"choices", json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}};
                    if (!extra.is_null()) c["bmoe"] = extra;
                    write("data: " + c.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n");
                };

                // Deltas, as the CLI's line protocol sends them: append the new tail; when the chat
                // parser reclassified text already sent (a closing reasoning tag), send the full state
                // with reset so the client replaces instead of appending.
                std::string sent_text, sent_reasoning;
                bool first = true;
                ChatOutcome out = app.host.chat(cr, [&](const TokenMetrics & m) {
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
                });
                if (out.status != 200) {
                    write("data: " +
                          json{{"error", {{"message", out.error}, {"code", out.status}}}}.dump(
                              -1, ' ', false, json::error_handler_t::replace) +
                          "\n\n");
                } else {
                    // The per-token text can lag the final answer (the chat parser holds back what it
                    // cannot classify yet), so close the gap before the last chunk: a client must end
                    // with exactly the answer the server will compare its next request against.
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
                    json extra = {{"summary", summary_json(out.result.summary, out.result.cancelled)}};
                    if (out.history_dropped) extra["history_dropped"] = true;
                    chunk(json::object(), out.finish_reason, extra);
                }
                write("data: [DONE]\n\n");
                sink.done();
                return true;
            });
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
        const std::string err = app.lib.start_download(body.value("id", ""));
        if (!err.empty()) return send_error(res, 404, err);
        send_json(res, json{{"ok", true}}, 202);
    });

    svr.Delete(R"(/api/models/download/(.+))", [&](const httplib::Request & req, httplib::Response & res) {
        const bool cancelled = app.lib.cancel_download(req.matches[1].str());
        send_json(res, json{{"ok", cancelled}}, cancelled ? 200 : 404);
    });

    svr.Get("/api/plan", [&](const httplib::Request &, httplib::Response & res) {
        if (!planner_available())
            return send_json(res, json{{"available", false},
                                       {"reason", "this build does not include the automatic hardware planner"}});
        if (app.host.generating())
            return send_error(res, 409, "busy: a plan measures the machine, and a running generation would skew it");
        PlanOutcome p = make_plan(app.store.config(), app.store.operator_keys());
        if (!p.ok) return send_json(res, json{{"available", true}, {"error", p.error}});
        const json body = p.body;
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

// A loopback server answers only to loopback names (a DNS-rebinding page cannot reach it through
// its own hostname), and a state-changing request from a browser must come from the UI's own
// origin (another site open in the same browser cannot drive the engine).
void register_guards(httplib::Server & svr, const Options & o) {
    std::set<std::string> origins = {"http://127.0.0.1:" + std::to_string(o.port),
                                     "http://localhost:" + std::to_string(o.port)};
    origins.insert(o.allow_origins.begin(), o.allow_origins.end());
    const bool loopback = is_loopback(o.host);
    svr.set_pre_routing_handler([origins, loopback](const httplib::Request & req, httplib::Response & res) {
        if (loopback) {
            std::string host = req.get_header_value("Host");
            const size_t colon = host.rfind(':');
            if (colon != std::string::npos && host.find(']') == std::string::npos) host = host.substr(0, colon);
            if (!host.empty() && !is_loopback(host) && host != "[::1]") {
                send_error(res, 403, "this server only answers to loopback host names");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        if (req.method != "GET" && req.method != "HEAD" && req.has_header("Origin") &&
            !origins.count(req.get_header_value("Origin"))) {
            send_error(res, 403, "cross-origin request refused (start the server with --allow-origin to permit one)");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
}

} // namespace

int main(int argc, char ** argv) {
    Options o;
    const int rc = parse_args(argc, argv, o);
    if (rc >= 0) return rc;

    const fs::path exe = fs::u8path(platform::executable_dir());
    const std::string data_dir = o.data_dir.empty() ? platform::default_data_dir() : o.data_dir;
    const std::string models_dir = o.models_dir.empty() ? (fs::u8path(data_dir) / "models").u8string() : o.models_dir;
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
    register_guards(svr, o);
    register_routes(svr, app);
    if (!ui_dir.empty()) {
        svr.set_mount_point("/", ui_dir);
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
    if (!is_loopback(o.host))
        std::printf("  WARNING: bound to %s: anyone who can reach this address can use the engine.\n", o.host.c_str());
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
