// A conversation whose agent is another program (docs/agent.md, "Other agents"): OpenCode, Claude
// Code, Codex or Pi, started as the catalog says and spoken to over the Agent Client Protocol. The
// program does the thinking and the tool calls; this file turns what it reports into the
// conversation's items, puts its permission requests in front of the person as the built-in agent's
// approval cards (the conversation's mode answering for them where it can), and serves its file
// reads and writes inside the workspace only.
#include "agent.h"
#include "agent_run.h"
#include "agent_tools.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace bmoe::server::agent {

// One external agent, kept for its conversation between messages.
struct Agent::External {
    std::unique_ptr<AcpConnection> conn;
    std::string agent; // its catalog id
    std::string session;
    std::string cwd; // the workspace, absolute
    std::mutex m;    // the fields below, written by the run's thread and read by the connection's
    Run * run = nullptr;
    std::string conv;                  // "" between messages: updates then (a replay) are not shown
    json message;                      // the assistant item being streamed; null when none
    std::map<std::string, json> tools; // tool items, by the agent's toolCallId
};

namespace {

// A LineProcess as the connection's transport.
class ProcessTransport : public AcpTransport {
public:
    explicit ProcessTransport(std::unique_ptr<platform::LineProcess> p) : p_(std::move(p)) {}
    bool write_line(const std::string & line) override { return p_->write_line(line); }
    bool read_line(std::string & line) override { return p_->read_line(line); }
    void stop() override { p_->stop(); }

private:
    std::unique_ptr<platform::LineProcess> p_;
};

// A tool call's state in the agent's words, as the item's.
std::string item_state(const std::string & status) {
    if (status == "completed") return "done";
    if (status == "failed") return "error";
    return "running"; // pending, in_progress
}

// What a tool call's content says, as the result text an item shows.
std::string content_text(const json & content) {
    std::string out;
    if (!content.is_array()) return out;
    for (const json & c : content) {
        const std::string type = c.value("type", "");
        std::string piece;
        if (type == "content" && c.contains("content") && c["content"].is_object())
            piece = c["content"].value("text", "");
        else if (type == "diff")
            piece = "Changed " + c.value("path", "");
        if (piece.empty()) continue;
        if (!out.empty()) out += "\n";
        out += piece;
    }
    return cap_text(out, k_tool_result_max_bytes);
}

// The first diff a tool call carries, as the approval card's diff and file.
void put_diff(json & item, const json & content, const std::string & cwd) {
    if (!content.is_array()) return;
    for (const json & c : content) {
        if (c.value("type", "") != "diff") continue;
        const std::string path = c.value("path", "");
        const std::string before =
            c.contains("oldText") && c["oldText"].is_string() ? c["oldText"].get<std::string>() : "";
        const std::string rel = workspace_relative(cwd, path);
        item["file"] = rel;
        item["diff"] = unified_diff(rel, before, c.value("newText", ""));
        return;
    }
}

// The option of a permission request with one of `kinds`, in that order of preference; "" for none.
std::string option_of(const json & options, std::initializer_list<const char *> kinds) {
    if (!options.is_array()) return "";
    for (const char * k : kinds)
        for (const json & o : options)
            if (o.value("kind", "") == k) return o.value("optionId", "");
    return "";
}

json selected(const std::string & option) {
    if (option.empty()) return {{"outcome", {{"outcome", "cancelled"}}}};
    return {{"outcome", {{"outcome", "selected"}, {"optionId", option}}}};
}

} // namespace

bool Agent::knows_agent(const std::string & id) const {
    if (id == k_builtin_agent) return true;
    for (const json & a : d_.external_agents)
        if (a.value("id", "") == id) return true;
    return false;
}

json Agent::capabilities() const {
    json agents = json::array();
    const bool commands = d_.tools && d_.can_run_commands;
    if (commands) {
        agents.push_back({{"id", k_builtin_agent}, {"name", "Built-in"}, {"available", true}});
        for (const json & a : d_.external_agents) {
            json missing = json::array();
            for (const json & p : a["acp"].value("needs", json::array()))
                if (p.is_string() && !(d_.program_found ? d_.program_found(p.get<std::string>())
                                                        : !platform::find_program(p.get<std::string>()).empty()))
                    missing.push_back(p);
            agents.push_back({{"id", a.value("id", "")},
                              {"name", a.value("name", "")},
                              {"available", missing.empty()},
                              {"missing", missing}});
        }
    }
    return {{"tools", d_.tools}, {"commands", commands}, {"agents", agents}};
}

void Agent::close_external(const std::string & conv_id) {
    std::shared_ptr<External> ext;
    {
        std::lock_guard<std::mutex> lk(m_);
        const auto it = externals_.find(conv_id);
        if (it == externals_.end()) return;
        ext = it->second;
        externals_.erase(it);
    }
    ext->conn.reset(); // ends the program and joins its threads
}

std::shared_ptr<Agent::External>
Agent::external_for(Run & run, const json & conv, const std::string & ws, std::string & err) {
    const std::string conv_id = conv.value("id", "");
    const std::string id = conv.value("agent", std::string(k_builtin_agent));
    {
        std::lock_guard<std::mutex> lk(m_);
        const auto it = externals_.find(conv_id);
        if (it != externals_.end() && it->second->conn->alive() && it->second->agent == id) return it->second;
    }
    close_external(conv_id); // a program that ended, or another agent: started afresh
    json entry;
    for (const json & a : d_.external_agents)
        if (a.value("id", "") == id) entry = a;
    if (entry.is_null()) {
        err = "there is no agent " + id + " here";
        return nullptr;
    }
    std::vector<std::string> command;
    for (const json & c : entry["acp"].value("command", json::array()))
        if (c.is_string()) command.push_back(c.get<std::string>());
    std::unique_ptr<AcpTransport> transport;
    if (d_.start_agent) {
        transport = d_.start_agent(command, ws, err);
    } else if (auto p = platform::LineProcess::start(command, ws, err)) {
        transport = std::make_unique<ProcessTransport>(std::move(p));
    }
    if (!transport) {
        err = entry.value("name", id) + " could not start: " + err;
        return nullptr;
    }
    auto ext = std::make_shared<External>();
    ext->agent = id;
    ext->cwd = ws;
    const std::weak_ptr<External> weak = ext;
    AcpConnection::Handlers h;
    h.notification = [this, weak](const std::string & method, const json & params) {
        if (method != "session/update") return;
        if (auto e = weak.lock()) on_external_update(e, params);
    };
    h.request = [this, weak](const std::string & method, const json & params, json & result, std::string & error) {
        auto e = weak.lock();
        if (!e) {
            error = "the conversation is gone";
            return false;
        }
        return on_external_request(e, method, params, result, error);
    };
    ext->conn = std::make_unique<AcpConnection>(std::move(transport), std::move(h));
    // A first start may download the agent (npx): the person can stop it meanwhile.
    const auto stopped = [&run] { return run.stop.load(); };
    json init;
    if (!ext->conn->call(
            "initialize",
            {{"protocolVersion", 1},
             {"clientCapabilities", {{"fs", {{"readTextFile", true}, {"writeTextFile", true}}}, {"terminal", false}}},
             {"clientInfo", {{"name", "bmoe-server"}, {"title", "BigMoeOnEdge"}}}},
            init, err, [&] {
                if (stopped()) ext->conn->close();
                return stopped();
            })) {
        err = entry.value("name", id) + " did not start: " + err;
        return nullptr;
    }
    const bool can_load = init.value("agentCapabilities", json::object()).value("loadSession", false);
    const std::string stored = conv.value("acp_session", "");
    const json session_params = {{"cwd", ws}, {"mcpServers", json::array()}};
    json res;
    if (!stored.empty() && can_load) {
        json p = session_params;
        p["sessionId"] = stored;
        if (ext->conn->call("session/load", p, res, err)) ext->session = stored;
    }
    if (ext->session.empty()) {
        if (!ext->conn->call("session/new", session_params, res, err) || !res.value("sessionId", json()).is_string()) {
            err = entry.value("name", id) + " did not open a session: " + (err.empty() ? "no session id" : err);
            return nullptr;
        }
        ext->session = res["sessionId"].get<std::string>();
        d_.store->edit_conversation(conv_id, [&](json & c) { c["acp_session"] = ext->session; });
        if (!stored.empty())
            notice(conv_id, "info",
                   entry.value("name", id) +
                       " started a new session: it does not remember this conversation's earlier messages.");
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        externals_[conv_id] = ext;
    }
    return ext;
}

std::string Agent::run_external(Run & run, const std::string & conv_id) {
    json conv;
    if (!d_.store->conversation(conv_id, conv)) return "";
    const std::string ws = workspace_path(conv);
    if (ws.empty() || run.remote || !d_.tools || !d_.can_run_commands) {
        notice(conv_id, "error",
               "This conversation's agent is another program: it needs a folder, and runs on this computer only.");
        run.failed = true;
        return "";
    }
    std::string text;
    for (const json & it : conv["items"])
        if (it.value("kind", "") == "user") text = it.value("text", "");
    std::string err;
    std::shared_ptr<External> ext = external_for(run, conv, ws, err);
    if (!ext) {
        if (run.stop.load()) {
            notice(conv_id, "info", "Stopped.");
        } else {
            notice(conv_id, "error", err);
            run.failed = true;
        }
        return "";
    }
    {
        std::lock_guard<std::mutex> lk(ext->m);
        ext->run = &run;
        ext->conv = conv_id;
        ext->message = nullptr;
        ext->tools.clear();
    }
    json result;
    std::chrono::steady_clock::time_point asked{};
    const bool ok = ext->conn->call(
        "session/prompt", {{"sessionId", ext->session}, {"prompt", json::array({{{"type", "text"}, {"text", text}}})}},
        result, err,
        [&] {
            if (!run.stop.load()) return false;
            // Asked to stop and still going past the grace: the program is ended.
            if (asked != std::chrono::steady_clock::time_point{} &&
                std::chrono::steady_clock::now() - asked > std::chrono::seconds(k_external_cancel_grace_s))
                ext->conn->close();
            return true;
        },
        [&] {
            asked = std::chrono::steady_clock::now();
            ext->conn->notify("session/cancel", {{"sessionId", ext->session}});
        });
    std::string final_text;
    {
        std::lock_guard<std::mutex> lk(ext->m);
        if (!ext->message.is_null()) {
            ext->message["streaming"] = false;
            final_text = ext->message.value("text", "");
            publish_item(conv_id, ext->message);
        }
        for (auto & [call, item] : ext->tools)
            if (item.value("state", "") == "running" || item.value("state", "") == "waiting") {
                item["state"] = "error";
                if (item.value("result", "").empty()) item["result"] = "The agent ended the turn before this finished.";
                publish_item(conv_id, item);
            }
        ext->run = nullptr;
        ext->conv.clear();
        ext->message = nullptr;
        ext->tools.clear();
    }
    if (run.stop.load()) {
        notice(conv_id, "info", "Stopped.");
    } else if (!ok) {
        notice(conv_id, "error", "The agent failed: " + err);
        run.failed = true;
    } else {
        const std::string why = result.value("stopReason", "end_turn");
        if (why == "max_tokens")
            notice(conv_id, "info", "The agent stopped at its token limit; send another message to go on.");
        else if (why == "max_turn_requests")
            notice(conv_id, "info", "The agent stopped at its limit of steps; send another message to go on.");
        else if (why == "refusal")
            notice(conv_id, "info", "The agent declined this request.");
    }
    if (!ext->conn->alive()) close_external(conv_id);
    return final_text;
}

void Agent::on_external_update(const std::shared_ptr<External> & ext, const json & params) {
    const json u = params.value("update", json::object());
    const std::string kind = u.value("sessionUpdate", "");
    std::lock_guard<std::mutex> lk(ext->m);
    if (ext->conv.empty()) return; // between messages (a session's replay): the items already hold it
    const std::string conv = ext->conv;
    if (kind == "agent_message_chunk" || kind == "agent_thought_chunk") {
        const json c = u.value("content", json::object());
        if (c.value("type", "") != "text") return;
        const std::string piece = c.value("text", "");
        if (piece.empty()) return;
        if (ext->message.is_null()) {
            ext->message =
                json{{"id", new_id("i")}, {"kind", "assistant"}, {"text", ""}, {"reasoning", ""}, {"streaming", true}};
            publish_item(conv, ext->message, false);
        }
        const char * field = kind == "agent_message_chunk" ? "text" : "reasoning";
        ext->message[field] = ext->message.value(field, "") + piece;
        d_.store->upsert_item(conv, ext->message, false);
        d_.bus->publish("delta", json{{"conversation", conv}, {"item", ext->message["id"]}, {field, piece}},
                        audience_of(conv));
    } else if (kind == "tool_call" || kind == "tool_call_update") {
        // A tool call closes the text before it, so the items read in the order things happened.
        if (!ext->message.is_null()) {
            ext->message["streaming"] = false;
            publish_item(conv, ext->message);
            ext->message = nullptr;
        }
        const std::string call = u.value("toolCallId", "");
        if (call.empty()) return;
        json & item = ext->tools[call];
        if (item.is_null())
            item = json{{"id", new_id("i")},      {"kind", "tool"},     {"tool", "other"}, {"title", ""},
                        {"args", json::object()}, {"state", "running"}, {"result", ""},    {"call_id", call}};
        if (u.contains("kind") && u["kind"].is_string()) item["tool"] = u["kind"];
        if (u.contains("title") && u["title"].is_string()) item["title"] = u["title"];
        if (u.contains("rawInput") && u["rawInput"].is_object()) item["args"] = u["rawInput"];
        if (u.contains("status") && u["status"].is_string() && item.value("state", "") != "waiting")
            item["state"] = item_state(u["status"].get<std::string>());
        if (u.contains("content")) {
            const std::string text = content_text(u["content"]);
            if (!text.empty()) item["result"] = text;
            put_diff(item, u["content"], ext->cwd);
        }
        publish_item(conv, item, item.value("state", "") != "running");
    }
    // plan, usage, commands and the rest: not shown.
}

bool Agent::on_external_request(const std::shared_ptr<External> & ext,
                                const std::string & method,
                                const json & params,
                                json & result,
                                std::string & error) {
    if (method == "session/request_permission") {
        Run * run = nullptr;
        std::string conv;
        json item;
        {
            std::lock_guard<std::mutex> lk(ext->m);
            run = ext->run;
            conv = ext->conv;
            if (!run) {
                result = selected("");
                return true;
            }
            const json tc = params.value("toolCall", json::object());
            const std::string call = tc.value("toolCallId", "");
            json & known = ext->tools[call.empty() ? new_id("t") : call];
            if (known.is_null())
                known = json{{"id", new_id("i")},      {"kind", "tool"},     {"tool", "other"}, {"title", ""},
                             {"args", json::object()}, {"state", "running"}, {"result", ""},    {"call_id", call}};
            if (tc.contains("title") && tc["title"].is_string()) known["title"] = tc["title"];
            if (tc.contains("kind") && tc["kind"].is_string()) known["tool"] = tc["kind"];
            if (tc.contains("rawInput") && tc["rawInput"].is_object()) known["args"] = tc["rawInput"];
            if (tc.contains("content")) put_diff(known, tc["content"], ext->cwd);
            item = known;
        }
        const json options = params.value("options", json::array());
        json c;
        d_.store->conversation(conv, c);
        const std::string mode = c.value("mode", "ask");
        bool allow = false;
        if (mode == "full")
            allow = true;
        else if (mode == "ask")
            allow = wait_approval(*run, conv, item);
        if (run->stop.load()) {
            result = selected("");
            return true;
        }
        item["state"] = allow ? "running" : "denied";
        {
            std::lock_guard<std::mutex> lk(ext->m);
            const std::string call = item.value("call_id", "");
            if (ext->tools.count(call)) ext->tools[call] = item;
        }
        publish_item(conv, item);
        result = selected(allow ? option_of(options, {"allow_once", "allow_always"})
                                : option_of(options, {"reject_once", "reject_always"}));
        return true;
    }
    if (method == "fs/read_text_file" || method == "fs/write_text_file") {
        std::string path, err;
        if (!resolve_in_workspace(ext->cwd, params.value("path", ""), path, err)) {
            error = err;
            return false;
        }
        if (method == "fs/read_text_file") {
            std::error_code ec;
            if (fs::file_size(fs::u8path(path), ec) > k_read_max_file_bytes || ec) {
                error = ec ? "no such file: " + params.value("path", "") : "the file is too large to read";
                return false;
            }
            std::ifstream f(fs::u8path(path), std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            std::string text = ss.str();
            // `line` (1-based) and `limit` choose lines, as the agent asked.
            const int first = params.value("line", 1);
            const int limit = params.value("limit", 0);
            if (first > 1 || limit > 0) {
                std::istringstream in(text);
                std::string l, out;
                int n = 0, taken = 0;
                while (std::getline(in, l)) {
                    if (++n < first) continue;
                    if (limit > 0 && taken >= limit) break;
                    out += l + "\n";
                    ++taken;
                }
                text = out;
            }
            result = {{"content", text}};
            return true;
        }
        json c;
        std::string conv;
        {
            std::lock_guard<std::mutex> lk(ext->m);
            conv = ext->conv;
        }
        if (!conv.empty() && d_.store->conversation(conv, c) && c.value("mode", "ask") == "read_only") {
            error = "this conversation is read only: nothing may be written";
            return false;
        }
        std::error_code ec;
        fs::create_directories(fs::u8path(path).parent_path(), ec);
        std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
        f << params.value("content", "");
        if (!f) {
            error = "could not write " + params.value("path", "");
            return false;
        }
        result = json::object();
        return true;
    }
    error = "method not found: " + method;
    return false;
}

} // namespace bmoe::server::agent
