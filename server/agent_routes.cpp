#include "agent_routes.h"

#include "access.h"
#include "agent_skills.h"
#include "agent_tools.h"
#include "connect.h"

#include <cpp-httplib/httplib.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace bmoe::server {

using namespace agent;

namespace {

void send_json(httplib::Response & res, const json & j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
}

void send_error(httplib::Response & res, int status, const std::string & msg, const std::string & code) {
    send_json(res, json{{"error", msg}, {"code", code}}, status);
}

void send_refusal(httplib::Response & res, const Refusal & r) {
    send_error(res, r.status, r.error, r.code);
}

bool body_of(const httplib::Request & req, httplib::Response & res, json & out) {
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

bool local(const httplib::Request & req) {
    return is_loopback_peer(req.remote_addr);
}

// A server started with --chat-only takes no agent work: no workspaces, tasks or tool runs.
bool refuse_chat_only(const AgentRouteDeps & d, httplib::Response & res) {
    if (d.agent->capabilities().value("tools", true)) return false;
    send_error(res, 409, "this server runs plain chat only (--chat-only): it has no agent", "chat_only");
    return true;
}

void refuse_remote(httplib::Response & res) {
    send_error(res, 403,
               "only a client on this computer may do this: it reaches this computer's files, processes or keys",
               k_local_only_code);
}

bool valid_mode(const std::string & m) {
    return m == "ask" || m == "read_only" || m == "full";
}

// A conversation that reaches this computer (conversation_reaches) answers to a local client only.
bool hidden_from(const httplib::Request & req, const json & conv) {
    return conversation_reaches(conv) && !local(req);
}

// `route` names the engine or a saved provider and one of its models. "" when it does.
std::string route_refusal(const AgentStore & store, const std::string & route) {
    if (route == "local") return "";
    const size_t slash = route.find('/');
    if (slash == std::string::npos || slash + 1 >= route.size())
        return "route must be \"local\" or \"<provider id>/<model>\"";
    json p;
    std::string key;
    if (!store.provider(route.substr(0, slash), p, key)) return "no saved provider " + route.substr(0, slash);
    return "";
}

// Reads the conversation fields a create or a change may set into `out`; false with the reason.
bool read_conversation_fields(
    const json & body, const AgentStore & store, const Agent & agent, json & out, std::string & err) {
    if (body.contains("title")) {
        if (!body["title"].is_string()) return err = "title must be a string", false;
        out["title"] = body["title"];
    }
    if (body.contains("mode")) {
        if (!body["mode"].is_string() || !valid_mode(body["mode"].get<std::string>()))
            return err = "mode must be ask, read_only or full", false;
        out["mode"] = body["mode"];
    }
    if (body.contains("route")) {
        if (!body["route"].is_string()) return err = "route must be a string", false;
        err = route_refusal(store, body["route"].get<std::string>());
        if (!err.empty()) return false;
        out["route"] = body["route"];
    }
    if (body.contains("workspace")) {
        json w;
        if (!body["workspace"].is_null() &&
            (!body["workspace"].is_string() || !store.workspace(body["workspace"].get<std::string>(), w)))
            return err = "no such workspace", false;
        out["workspace"] = body["workspace"];
    }
    if (body.contains("agent")) {
        // Another agent works in a folder of this computer: it needs one, and a computer that runs
        // programs.
        if (!body["agent"].is_string() || !agent.knows_agent(body["agent"].get<std::string>()))
            return err = "no such agent", false;
        out["agent"] = body["agent"];
    }
    return true;
}

const char * content_type_of(const std::string & path) {
    static const std::pair<const char *, const char *> types[] = {{".html", "text/html; charset=utf-8"},
                                                                  {".htm", "text/html; charset=utf-8"},
                                                                  {".css", "text/css"},
                                                                  {".js", "text/javascript"},
                                                                  {".json", "application/json"},
                                                                  {".md", "text/markdown; charset=utf-8"},
                                                                  {".png", "image/png"},
                                                                  {".jpg", "image/jpeg"},
                                                                  {".jpeg", "image/jpeg"},
                                                                  {".gif", "image/gif"},
                                                                  {".webp", "image/webp"},
                                                                  {".svg", "image/svg+xml"},
                                                                  {".pdf", "application/pdf"},
                                                                  {".csv", "text/csv; charset=utf-8"}};
    std::string ext = fs::u8path(path).extension().u8string();
    for (char & c : ext)
        c = (char) std::tolower((unsigned char) c);
    for (const auto & [e, t] : types)
        if (ext == e) return t;
    return "text/plain; charset=utf-8";
}

json skill_json(const Skill & s, bool enabled) {
    return json{{"name", s.name},     {"title", s.title},   {"description", s.description},
                {"source", s.source}, {"enabled", enabled}, {"body", s.body}};
}

// A provider's id from its name: lowercase letters, digits and hyphens, so a route `<id>/<model>`
// splits at the first slash.
std::string slug(const std::string & name) {
    std::string out;
    for (char c : name) {
        const unsigned char u = (unsigned char) c;
        if (std::isalnum(u))
            out += (char) std::tolower(u);
        else if (!out.empty() && out.back() != '-')
            out += '-';
    }
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    return out.empty() ? "provider" : out.substr(0, 40);
}

bool read_provider(const json & body, json & out, std::string & err) {
    const std::string name = body.value("name", out.value("name", ""));
    const std::string base = body.value("base_url", out.value("base_url", ""));
    if (name.empty()) return err = "name is required", false;
    if (base.compare(0, 8, "https://") != 0 && base.compare(0, 7, "http://") != 0)
        return err = "base_url must be an http(s) URL, such as https://api.example.com/v1", false;
    json models = body.contains("models") ? body["models"] : out.value("models", json::array());
    if (!models.is_array()) return err = "models must be an array of model names", false;
    for (const json & m : models)
        if (!m.is_string() || m.get<std::string>().empty())
            return err = "models must be an array of model names", false;
    out["name"] = name;
    out["base_url"] = base;
    out["models"] = models;
    return true;
}

bool read_task(const json & body, const AgentStore & store, const Agent & agent, json & task, std::string & err) {
    for (const char * k : {"title", "prompt"})
        if (body.contains(k)) {
            if (!body[k].is_string()) return err = std::string(k) + " must be a string", false;
            task[k] = body[k];
        }
    if (task.value("prompt", "").empty()) return err = "prompt is required", false;
    if (task.value("title", "").empty()) task["title"] = task.value("prompt", "").substr(0, k_title_max_chars);
    json fields = json::object();
    if (!read_conversation_fields(body, store, agent, fields, err)) return false;
    for (auto it = fields.begin(); it != fields.end(); ++it)
        if (it.key() != "title") task[it.key()] = it.value();
    if (!task.contains("mode")) task["mode"] = "read_only";
    if (!task.contains("route")) task["route"] = "local";
    if (!task.contains("workspace")) task["workspace"] = nullptr;
    if (body.contains("schedule")) {
        Schedule s;
        if (!parse_schedule(body["schedule"], s, err)) return false;
        task["schedule"] = schedule_json(s);
    }
    if (!task.contains("schedule")) return err = "schedule is required", false;
    if (body.contains("enabled")) {
        if (!body["enabled"].is_boolean()) return err = "enabled must be a boolean", false;
        task["enabled"] = body["enabled"];
    }
    if (!task.contains("enabled")) task["enabled"] = true;
    return true;
}

} // namespace

void register_agent_routes(httplib::Server & svr, const AgentRouteDeps & d) {
    svr.Get("/api/agent",
            [&d](const httplib::Request &, httplib::Response & res) { send_json(res, d.agent->capabilities()); });

    // ---- workspaces ----
    svr.Get("/api/workspaces",
            [&d](const httplib::Request &, httplib::Response & res) { send_json(res, d.store->workspaces()); });
    svr.Post("/api/workspaces", [&d](const httplib::Request & req, httplib::Response & res) {
        if (refuse_chat_only(d, res)) return;
        json body;
        if (!body_of(req, res, body)) return;
        json w;
        std::string err;
        if (!d.store->add_workspace(body.value("path", ""), body.value("name", ""), w, err))
            return send_error(res, 400, err, "not_a_folder");
        send_json(res, w);
    });
    svr.Delete(R"(/api/workspaces/([^/]+))", [&d](const httplib::Request & req, httplib::Response & res) {
        if (!d.store->remove_workspace(req.matches[1].str()))
            return send_error(res, 404, "no such workspace", "not_found");
        send_json(res, json{{"ok", true}});
    });

    // ---- conversations ----
    svr.Get("/api/conversations", [&d](const httplib::Request & req, httplib::Response & res) {
        json list = d.agent->list_conversations();
        if (!local(req)) {
            json shown = json::array();
            for (const json & c : list)
                if (!conversation_reaches(c)) shown.push_back(c);
            list = std::move(shown);
        }
        send_json(res, list);
    });
    svr.Post("/api/conversations", [&d](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!body_of(req, res, body)) return;
        json fields = json::object();
        std::string err;
        if (!read_conversation_fields(body, *d.store, *d.agent, fields, err))
            return send_error(res, 400, err, "invalid_request");
        if (!local(req) && network_may_not_set(fields)) return refuse_remote(res);
        if (fields.value("agent", std::string(agent::k_builtin_agent)) != agent::k_builtin_agent &&
            !fields.value("workspace", json(nullptr)).is_string())
            return send_error(res, 400, "another agent works in a folder: pick a workspace for it", "invalid_request");
        if (body.contains("items")) {
            // A conversation the UI kept before the server did: its messages, as they were.
            if (!body["items"].is_array()) return send_error(res, 400, "items must be an array", "invalid_request");
            json items = json::array();
            for (const json & it : body["items"]) {
                const std::string kind = it.is_object() ? it.value("kind", "") : "";
                if ((kind != "user" && kind != "assistant") || !it.contains("text") || !it["text"].is_string())
                    return send_error(res, 400, "an imported item is {kind: user | assistant, text}",
                                      "invalid_request");
                json item{{"id", new_id("i")}, {"kind", kind}, {"text", it["text"]}};
                if (kind == "assistant") {
                    item["reasoning"] = "";
                    item["streaming"] = false;
                }
                items.push_back(item);
            }
            fields["items"] = items;
        }
        json c = d.store->create_conversation(fields);
        d.bus->publish("conversation",
                       json{{"id", c["id"]},
                            {"title", c["title"]},
                            {"state", "idle"},
                            {"updated", c["updated"]},
                            {"parent", nullptr}},
                       conversation_audience(c));
        send_json(res, c);
    });
    svr.Get(R"(/api/conversations/([^/]+))", [&d](const httplib::Request & req, httplib::Response & res) {
        json c;
        if (!d.agent->conversation(req.matches[1].str(), c))
            return send_error(res, 404, "no such conversation", "not_found");
        if (hidden_from(req, c)) return refuse_remote(res);
        send_json(res, c);
    });
    svr.Patch(R"(/api/conversations/([^/]+))", [&d](const httplib::Request & req, httplib::Response & res) {
        const std::string id = req.matches[1].str();
        json body;
        if (!body_of(req, res, body)) return;
        json fields = json::object();
        std::string err;
        if (!read_conversation_fields(body, *d.store, *d.agent, fields, err))
            return send_error(res, 400, err, "invalid_request");
        if (!local(req)) {
            json before;
            if (!d.store->conversation(id, before)) return send_error(res, 404, "no such conversation", "not_found");
            if (hidden_from(req, before) || network_may_not_set(fields)) return refuse_remote(res);
        }
        // The mode may change while a run goes (a person allows everything while a card waits); what
        // the run works in and with may not.
        if ((fields.contains("workspace") || fields.contains("route")) && d.agent->running(id))
            return send_error(res, 409, "this conversation is running: stop it first", "busy");
        if (fields.contains("agent")) {
            // The agent is chosen before the first message: another one would not know what was said.
            json before;
            if (d.store->conversation(id, before) && !before["items"].empty() &&
                before.value("agent", std::string(agent::k_builtin_agent)) != fields.value("agent", ""))
                return send_error(res, 409, "the agent is chosen before the first message: start a new conversation",
                                  "busy");
            const bool external = fields.value("agent", "") != agent::k_builtin_agent;
            const json ws =
                fields.contains("workspace") ? fields["workspace"] : before.value("workspace", json(nullptr));
            if (external && !ws.is_string())
                return send_error(res, 400, "another agent works in a folder: pick a workspace for it",
                                  "invalid_request");
        }
        json after;
        if (!d.store->edit_conversation(
                id,
                [&](json & c) {
                    for (auto it = fields.begin(); it != fields.end(); ++it)
                        c[it.key()] = it.value();
                },
                true, &after))
            return send_error(res, 404, "no such conversation", "not_found");
        json view;
        d.agent->conversation(id, view);
        d.bus->publish("conversation",
                       json{{"id", id},
                            {"title", view.value("title", "")},
                            {"state", view.value("state", "idle")},
                            {"updated", view.value("updated", 0LL)},
                            {"parent", view.value("parent", json(nullptr))}},
                       conversation_audience(view));
        view.erase("items");
        send_json(res, view);
    });
    svr.Delete(R"(/api/conversations/([^/]+))", [&d](const httplib::Request & req, httplib::Response & res) {
        json conv;
        if (d.store->conversation(req.matches[1].str(), conv) && hidden_from(req, conv)) return refuse_remote(res);
        const Refusal r = d.agent->remove_conversation(req.matches[1].str());
        if (!r.ok()) return send_refusal(res, r);
        send_json(res, json{{"ok", true}});
    });

    const auto message = [&d](const httplib::Request & req, httplib::Response & res, bool edit) {
        const std::string id = req.matches[1].str();
        json body;
        if (!body_of(req, res, body)) return;
        json conv;
        if (!d.store->conversation(id, conv)) return send_error(res, 404, "no such conversation", "not_found");
        if (hidden_from(req, conv)) return refuse_remote(res);
        if (!body.contains("text") || !body["text"].is_string() || body["text"].get<std::string>().empty())
            return send_error(res, 400, "text must be a non-empty string", "invalid_request");
        if (body.contains("images") && body["images"].is_array() && !body["images"].empty())
            return send_error(res, 400, "images are not supported by the agent yet", "unsupported");
        const bool think = body.contains("think") && body["think"].is_boolean() && body["think"].get<bool>();
        json item;
        const Refusal r =
            edit ? d.agent->edit(id, body.value("item", ""), body["text"].get<std::string>(), think, !local(req), item)
                 : d.agent->send(id, body["text"].get<std::string>(), think, !local(req), item);
        if (!r.ok()) return send_refusal(res, r);
        send_json(res, json{{"item", item}}, 202);
    };
    svr.Post(R"(/api/conversations/([^/]+)/messages)",
             [message](const httplib::Request & req, httplib::Response & res) { message(req, res, false); });
    svr.Post(R"(/api/conversations/([^/]+)/edit)",
             [message](const httplib::Request & req, httplib::Response & res) { message(req, res, true); });
    svr.Post(R"(/api/conversations/([^/]+)/approval)", [&d](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!body_of(req, res, body)) return;
        const std::string decision = body.value("decision", "");
        if (decision != "approve" && decision != "deny")
            return send_error(res, 400, "decision must be approve or deny", "invalid_request");
        const Refusal r = d.agent->approve(req.matches[1].str(), body.value("item", ""), decision == "approve");
        if (!r.ok()) return send_refusal(res, r);
        send_json(res, json{{"ok", true}});
    });
    svr.Post(R"(/api/conversations/([^/]+)/stop)", [&d](const httplib::Request & req, httplib::Response & res) {
        json conv;
        if (d.store->conversation(req.matches[1].str(), conv) && hidden_from(req, conv)) return refuse_remote(res);
        send_json(res, json{{"stopped", d.agent->stop(req.matches[1].str())}});
    });
    svr.Get(R"(/api/conversations/([^/]+)/file)", [&d](const httplib::Request & req, httplib::Response & res) {
        json conv;
        if (!d.store->conversation(req.matches[1].str(), conv))
            return send_error(res, 404, "no such conversation", "not_found");
        if (!local(req)) return refuse_remote(res);
        json w;
        if (!conv.value("workspace", json(nullptr)).is_string() ||
            !d.store->workspace(conv["workspace"].get<std::string>(), w))
            return send_error(res, 404, "this conversation has no workspace", "not_found");
        std::string path, err;
        if (!resolve_in_workspace(w.value("path", ""), req.get_param_value("path"), path, err))
            return send_error(res, 403, err, "outside_workspace");
        std::error_code ec;
        if (!fs::is_regular_file(fs::u8path(path), ec)) return send_error(res, 404, "no such file", "not_found");
        if (fs::file_size(fs::u8path(path), ec) > k_preview_max_bytes)
            return send_error(res, 413, "the file is too large to preview", "too_large");
        std::ifstream f(fs::u8path(path), std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        res.set_content(ss.str(), content_type_of(path));
        // Whatever the file holds is shown, never run as part of this origin.
        res.set_header("Content-Security-Policy",
                       "sandbox; default-src 'none'; img-src 'self' data:; style-src 'unsafe-inline'");
        res.set_header("Content-Disposition", "inline");
    });

    // ---- skills ----
    svr.Get("/api/skills", [&d](const httplib::Request & req, httplib::Response & res) {
        std::string ws;
        if (req.has_param("workspace")) {
            json w;
            if (d.store->workspace(req.get_param_value("workspace"), w)) ws = w.value("path", "");
        }
        json out = json::array();
        for (const auto & [s, on] : d.agent->skills(ws))
            out.push_back(skill_json(s, on));
        send_json(res, out);
    });
    const auto write_skill = [&d](const httplib::Request & req, httplib::Response & res, const std::string & name,
                                  bool create) {
        json body;
        if (!body_of(req, res, body)) return;
        Skill s;
        s.name = name.empty() ? body.value("name", "") : name;
        s.description = body.value("description", "");
        s.title = body.value("title", "");
        s.body = body.value("body", "");
        if (!valid_skill_name(s.name))
            return send_error(res, 400, "name must be kebab-case: lowercase letters, digits and hyphens",
                              "invalid_request");
        if (s.description.empty() || s.description.size() > k_skill_description_max)
            return send_error(res, 400, "description is required (it is how the agent knows when the skill helps)",
                              "invalid_request");
        if (s.body.size() > k_skill_body_max)
            return send_error(res, 400, "the instructions are too long", "invalid_request");
        const fs::path dir = fs::u8path(d.user_skills_dir) / s.name;
        std::error_code ec;
        if (create && fs::exists(dir / "SKILL.md", ec))
            return send_error(res, 409, "a skill of yours already has that name", "exists");
        std::string err;
        if (!write_file_atomic((dir / "SKILL.md").u8string(), render_skill(s), false, err))
            return send_error(res, 500, err, "failed");
        Skill back;
        parse_skill(render_skill(s), back, err);
        back.source = "user";
        send_json(res,
                  skill_json(back, d.store->skill_switches().count(s.name) ? d.store->skill_switches()[s.name] : true));
    };
    svr.Post("/api/skills",
             [write_skill](const httplib::Request & req, httplib::Response & res) { write_skill(req, res, "", true); });
    svr.Put(R"(/api/skills/([^/]+))", [write_skill](const httplib::Request & req, httplib::Response & res) {
        write_skill(req, res, req.matches[1].str(), false);
    });
    svr.Patch(R"(/api/skills/([^/]+))", [&d](const httplib::Request & req, httplib::Response & res) {
        json body;
        if (!body_of(req, res, body)) return;
        if (!body.contains("enabled") || !body["enabled"].is_boolean())
            return send_error(res, 400, "enabled must be a boolean", "invalid_request");
        const std::string name = req.matches[1].str();
        if (!valid_skill_name(name)) return send_error(res, 404, "no such skill", "not_found");
        d.store->set_skill_enabled(name, body["enabled"].get<bool>());
        send_json(res, json{{"name", name}, {"enabled", body["enabled"]}});
    });
    svr.Delete(R"(/api/skills/([^/]+))", [&d](const httplib::Request & req, httplib::Response & res) {
        const std::string name = req.matches[1].str();
        std::error_code ec;
        const fs::path dir = fs::u8path(d.user_skills_dir) / name;
        if (!valid_skill_name(name) || !fs::exists(dir / "SKILL.md", ec))
            return send_error(res, 404,
                              "no skill of yours has that name (a bundled skill can be switched off, not deleted)",
                              "not_found");
        fs::remove_all(dir, ec);
        if (ec) return send_error(res, 500, "could not delete the skill: " + ec.message(), "failed");
        send_json(res, json{{"ok", true}});
    });

    // ---- tasks ----
    svr.Get("/api/tasks", [&d](const httplib::Request &, httplib::Response & res) {
        json out = json::array();
        for (const json & t : d.store->tasks())
            out.push_back(d.agent->task_view(t));
        send_json(res, out);
    });
    const auto put_task = [&d](const httplib::Request & req, httplib::Response & res, const std::string & id) {
        json body;
        if (!body_of(req, res, body)) return;
        json task;
        if (!id.empty() && !d.store->task(id, task)) return send_error(res, 404, "no such task", "not_found");
        if (id.empty()) task = json{{"id", new_id("t")}, {"created", now_s()}, {"last_run", nullptr}};
        std::string err;
        if (!read_task(body, *d.store, *d.agent, task, err)) return send_error(res, 400, err, "invalid_request");
        d.store->put_task(task);
        d.agent->tasks_changed();
        const json view = d.agent->task_view(task);
        d.bus->publish("task", view, EventBus::Audience::LocalOnly);
        send_json(res, view);
    };
    svr.Post("/api/tasks", [&d, put_task](const httplib::Request & req, httplib::Response & res) {
        if (!refuse_chat_only(d, res)) put_task(req, res, "");
    });
    svr.Put(R"(/api/tasks/([^/]+))", [&d, put_task](const httplib::Request & req, httplib::Response & res) {
        if (!refuse_chat_only(d, res)) put_task(req, res, req.matches[1].str());
    });
    svr.Delete(R"(/api/tasks/([^/]+))", [&d](const httplib::Request & req, httplib::Response & res) {
        if (!d.store->remove_task(req.matches[1].str())) return send_error(res, 404, "no such task", "not_found");
        d.agent->tasks_changed();
        send_json(res, json{{"ok", true}});
    });
    svr.Post(R"(/api/tasks/([^/]+)/run)", [&d](const httplib::Request & req, httplib::Response & res) {
        if (refuse_chat_only(d, res)) return;
        const Refusal r = d.agent->run_task(req.matches[1].str());
        if (!r.ok()) return send_refusal(res, r);
        json t;
        d.store->task(req.matches[1].str(), t);
        send_json(res, d.agent->task_view(t), 202);
    });

    // ---- providers ----
    svr.Get("/api/providers",
            [&d](const httplib::Request &, httplib::Response & res) { send_json(res, d.store->providers()); });
    const auto put_provider = [&d](const httplib::Request & req, httplib::Response & res, const std::string & id) {
        json body;
        if (!body_of(req, res, body)) return;
        json p = json::object();
        std::string key;
        if (!id.empty() && !d.store->provider(id, p, key)) return send_error(res, 404, "no such provider", "not_found");
        std::string err;
        if (!read_provider(body, p, err)) return send_error(res, 400, err, "invalid_request");
        if (id.empty()) {
            // The id stays what it was first made from: a route names it in saved conversations.
            std::string base = slug(p.value("name", "")), pid = base;
            json other;
            for (int n = 2; d.store->provider(pid, other, key); ++n)
                pid = base + "-" + std::to_string(n);
            p["id"] = pid;
        }
        if (body.contains("api_key") && !body["api_key"].is_string() && !body["api_key"].is_null())
            return send_error(res, 400, "api_key must be a string", "invalid_request");
        std::string new_key;
        const bool set_key = body.contains("api_key") && body["api_key"].is_string();
        if (set_key) new_key = body["api_key"].get<std::string>();
        if (!d.store->put_provider(p, set_key ? &new_key : nullptr, err)) return send_error(res, 500, err, "failed");
        json view;
        d.store->provider(p.value("id", ""), view, key);
        send_json(res, view);
    };
    svr.Post("/api/providers",
             [put_provider](const httplib::Request & req, httplib::Response & res) { put_provider(req, res, ""); });
    svr.Put(R"(/api/providers/([^/]+))", [put_provider](const httplib::Request & req, httplib::Response & res) {
        put_provider(req, res, req.matches[1].str());
    });
    svr.Delete(R"(/api/providers/([^/]+))", [&d](const httplib::Request & req, httplib::Response & res) {
        if (!d.store->remove_provider(req.matches[1].str()))
            return send_error(res, 404, "no such provider", "not_found");
        send_json(res, json{{"ok", true}});
    });

    // ---- connecting other apps ----
    const auto facts = [&d] {
        ConnectFacts f;
        f.origin = d.origin;
        f.has_key = d.has_key;
        f.model = d.host->model_name();
        f.home = d.home;
        return f;
    };
    svr.Get("/api/connect", [&d, facts](const httplib::Request &, httplib::Response & res) {
        send_json(res, connect_list(d.clients, facts()));
    });
    svr.Post(R"(/api/connect/([^/]+))", [&d, facts](const httplib::Request & req, httplib::Response & res) {
        json result;
        int status = 0;
        std::string err;
        if (!connect_write(d.clients, req.matches[1].str(), facts(), result, status, err))
            return send_error(res, status, err, status == 404 ? "not_found" : status == 409 ? "conflict" : "failed");
        send_json(res, result);
    });
}

} // namespace bmoe::server
