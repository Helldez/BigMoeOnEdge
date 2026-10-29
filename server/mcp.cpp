#include "mcp.h"

#include "platform.h"

#include <chrono>

namespace bmoe::server {

namespace {

using Clock = std::chrono::steady_clock;

std::string rpc_error(const std::string & err) {
    return err.empty() ? std::string("no answer") : err;
}

// A tools/call result as the model reads it: the text parts of `content`, joined; a part of another
// kind (an image, a resource) is named, since the model cannot see it.
std::string content_text(const json & result) {
    std::string out;
    if (!result.contains("content") || !result["content"].is_array()) return out;
    for (const json & part : result["content"]) {
        if (!part.is_object()) continue;
        const std::string type = part.value("type", "");
        std::string piece = type == "text" ? part.value("text", "") : "[" + type + " content not shown]";
        if (piece.empty()) continue;
        if (!out.empty()) out += "\n";
        out += piece;
    }
    return out;
}

} // namespace

std::vector<McpServerConfig> parse_mcp_config(const json & j, std::vector<std::string> & warnings) {
    std::vector<McpServerConfig> out;
    if (!j.is_object() || !j.contains("mcpServers") || !j["mcpServers"].is_object()) {
        warnings.push_back("the MCP config has no \"mcpServers\" object");
        return out;
    }
    for (const auto & [name, cfg] : j["mcpServers"].items()) {
        if (!cfg.is_object()) continue;
        McpServerConfig c;
        c.name = name;
        const std::string command = cfg.value("command", std::string());
        if (command.empty()) {
            warnings.push_back(
                "MCP server " + name +
                (cfg.contains("url") ? " is remote (url): only stdio servers are started" : " has no command"));
            continue;
        }
        c.command.push_back(command);
        if (cfg.contains("args") && cfg["args"].is_array())
            for (const json & a : cfg["args"])
                if (a.is_string()) c.command.push_back(a.get<std::string>());
        if (cfg.contains("env") && cfg["env"].is_object())
            for (const auto & [k, v] : cfg["env"].items())
                if (v.is_string()) c.env[k] = v.get<std::string>();
        c.cwd = cfg.value("cwd", std::string());
        c.timeout_ms = cfg.value("timeout_ms", k_mcp_call_timeout_ms);
        if (c.timeout_ms <= 0) c.timeout_ms = k_mcp_call_timeout_ms;
        out.push_back(std::move(c));
    }
    return out;
}

McpHub::McpHub(std::vector<McpServerConfig> servers, StartFn start) : start_(std::move(start)) {
    for (McpServerConfig & c : servers) {
        auto s = std::make_unique<Server>();
        s->config = std::move(c);
        servers_.push_back(std::move(s));
    }
}

McpHub::~McpHub() {
    shutdown();
}

McpHub::Server * McpHub::find(const std::string & name) {
    for (auto & s : servers_)
        if (s->config.name == name) return s.get();
    return nullptr;
}

bool McpHub::ensure(Server & s, const std::function<bool()> & stopped) {
    if (s.conn && s.conn->alive()) return true;
    if (s.failed || shut_) return false;
    // A program that ended (a stop, a time limit, its own end) is started again; one that never
    // started is not, so a broken entry costs one wait, not one per request.
    const bool first = !s.conn;
    s.conn.reset();
    s.tools.clear();
    {
        std::lock_guard<std::mutex> lk(s.listed_m);
        s.listed.clear();
    }
    std::string err;
    std::unique_ptr<JsonRpcTransport> transport;
    if (start_) {
        transport = start_(s.config, err);
    } else if (auto p = platform::LineProcess::start(s.config.command, s.config.cwd, err, s.config.env)) {
        transport = std::make_unique<ProcessTransport>(std::move(p));
    }
    const auto give_up = [&](const std::string & why) {
        s.conn.reset();
        if (first) {
            s.failed = true;
            s.error = why;
        }
        return false;
    };
    if (!transport) return give_up("could not start: " + err);
    JsonRpcConnection::Handlers h;
    // A server may ask the client things (ping, roots, sampling): a ping is answered, the rest is
    // refused as unknown, which the protocol allows for a client that declares no such capability.
    h.request = [](const std::string & method, const json &, json & result, std::string & error) {
        if (method == "ping") {
            result = json::object();
            return true;
        }
        error = "method not found: " + method;
        return false;
    };
    s.conn = std::make_unique<JsonRpcConnection>(std::move(transport), std::move(h));
    const auto deadline = Clock::now() + std::chrono::milliseconds(k_mcp_start_timeout_ms);
    bool late = false;
    const auto cancelled = [&] {
        late = Clock::now() >= deadline;
        if (late || shut_ || (stopped && stopped())) {
            s.conn->close();
            return true;
        }
        return false;
    };
    json res;
    if (!s.conn->call("initialize",
                      {{"protocolVersion", k_mcp_protocol_version},
                       {"capabilities", json::object()},
                       {"clientInfo", {{"name", "bmoe-server"}, {"version", "1.0"}}}},
                      res, err, cancelled))
        return give_up(late ? "did not answer initialize in " + std::to_string(k_mcp_start_timeout_ms / 1000) + " s"
                            : "initialize failed: " + rpc_error(err));
    s.conn->notify("notifications/initialized", json::object());
    // tools/list may come in pages.
    std::string cursor;
    for (int page = 0; page < 100; ++page) {
        json params = json::object();
        if (!cursor.empty()) params["cursor"] = cursor;
        if (!s.conn->call("tools/list", params, res, err, cancelled))
            return give_up(late ? "did not list its tools in " + std::to_string(k_mcp_start_timeout_ms / 1000) + " s"
                                : "tools/list failed: " + rpc_error(err));
        if (res.contains("tools") && res["tools"].is_array())
            for (const json & t : res["tools"]) {
                McpTool tool;
                tool.server = s.config.name;
                tool.name = t.value("name", "");
                tool.description = t.value("description", "");
                if (t.contains("inputSchema") && t["inputSchema"].is_object()) tool.input_schema = t["inputSchema"];
                if (!tool.name.empty()) s.tools.push_back(std::move(tool));
            }
        cursor = res.contains("nextCursor") && res["nextCursor"].is_string() ? res["nextCursor"].get<std::string>()
                                                                             : std::string();
        if (cursor.empty()) break;
    }
    s.error.clear();
    std::lock_guard<std::mutex> lk(s.listed_m);
    s.listed = s.tools;
    return true;
}

std::vector<McpTool> McpHub::tools(const std::function<bool()> & stopped) {
    std::vector<McpTool> out;
    for (auto & s : servers_) {
        std::unique_lock<std::mutex> lk(s->m, std::try_to_lock);
        if (lk.owns_lock() && !ensure(*s, stopped)) continue;
        // Busy with a call: running, so the tools it listed still hold.
        std::lock_guard<std::mutex> lk2(s->listed_m);
        out.insert(out.end(), s->listed.begin(), s->listed.end());
    }
    return out;
}

bool McpHub::call(const std::string & server,
                  const std::string & tool,
                  const json & arguments,
                  std::string & text,
                  const std::function<bool()> & stopped) {
    Server * s = find(server);
    if (!s) {
        text = "there is no MCP server " + server;
        return false;
    }
    std::lock_guard<std::mutex> lk(s->m);
    if (!ensure(*s, stopped)) {
        text = "the MCP server " + server + " is not running" + (s->error.empty() ? "" : ": " + s->error);
        return false;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(s->config.timeout_ms);
    bool late = false, halted = false;
    json res;
    std::string err;
    const bool ok =
        s->conn->call("tools/call", {{"name", tool}, {"arguments", arguments.is_object() ? arguments : json::object()}},
                      res, err, [&] {
                          late = Clock::now() >= deadline;
                          halted = shut_ || (stopped && stopped());
                          // The protocol has no way to take a call back that every server honours: the program is
                          // ended, and started again by the next call.
                          if (late || halted) s->conn->close();
                          return late || halted;
                      });
    if (!ok) {
        text = halted ? "stopped"
               : late ? "the MCP server did not answer in " + std::to_string(s->config.timeout_ms / 1000) + " s"
                      : rpc_error(err);
        return false;
    }
    text = content_text(res);
    if (res.value("isError", false)) {
        if (text.empty()) text = "the tool reported an error";
        return false;
    }
    if (text.empty() && res.contains("structuredContent")) text = res["structuredContent"].dump();
    return true;
}

json McpHub::describe() {
    json out = json::array();
    for (auto & s : servers_) {
        std::lock_guard<std::mutex> lk(s->m);
        ensure(*s, nullptr);
        json tools = json::array();
        for (const McpTool & t : s->tools)
            tools.push_back({{"name", t.exposed_name()}, {"description", t.description}});
        json e{{"name", s->config.name},
               {"command", s->config.command.empty() ? "" : s->config.command[0]},
               {"state", s->failed                       ? "failed"
                         : (s->conn && s->conn->alive()) ? "running"
                                                         : "stopped"},
               {"tools", tools}};
        if (!s->error.empty()) e["error"] = s->error;
        out.push_back(e);
    }
    return out;
}

void McpHub::shutdown() {
    shut_ = true;
    for (auto & s : servers_) {
        std::lock_guard<std::mutex> lk(s->m);
        s->conn.reset(); // ends the program and joins its threads
    }
}

} // namespace bmoe::server
