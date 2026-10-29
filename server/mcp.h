// Tool servers over the Model Context Protocol (docs/agent.md, "MCP servers"): programs named in a
// config file in the common `mcpServers` format, started the first time their tools are wanted and
// kept for the server's life, spoken to over stdio (JSON-RPC, one message a line: json_rpc.h). Each
// server's tools reach the built-in agent as `<server>_<tool>`; what may call one, and when, is the
// agent's rule (agent.cpp), not this file's.
#pragma once

#include "json_rpc.h"
#include "json_util.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bmoe::server {

// The protocol version the client asks for in `initialize`.
inline constexpr const char * k_mcp_protocol_version = "2024-11-05";
// How long one call may take before the server is ended (and started again at the next call).
inline constexpr int k_mcp_call_timeout_ms = 30000;
// How long a server has to start and list its tools: one that hangs must not hold up a request.
inline constexpr int k_mcp_start_timeout_ms = 10000;

struct McpServerConfig {
    std::string name;
    std::vector<std::string> command; // the program, then its arguments
    std::map<std::string, std::string> env;
    std::string cwd;
    int timeout_ms = k_mcp_call_timeout_ms;
};

// `{"mcpServers": {"<name>": {"command", "args", "env", "cwd"?, "timeout_ms"?}}}`. An entry with no
// command, or a remote one (`url`: only stdio is spoken), is left out with a warning.
std::vector<McpServerConfig> parse_mcp_config(const json & j, std::vector<std::string> & warnings);

struct McpTool {
    std::string server;
    std::string name;
    std::string description;
    json input_schema = json::object();
    // What the model calls it: the server's name, '_', the tool's.
    std::string exposed_name() const { return server + "_" + name; }
};

class McpHub {
public:
    // Starts one server's program; a test passes an in-process server. Null: platform::LineProcess.
    using StartFn = std::function<std::unique_ptr<JsonRpcTransport>(const McpServerConfig &, std::string & err)>;

    explicit McpHub(std::vector<McpServerConfig> servers, StartFn start = nullptr);
    ~McpHub();
    McpHub(const McpHub &) = delete;
    McpHub & operator=(const McpHub &) = delete;

    bool empty() const { return servers_.empty(); }
    // Every running server's tools, starting each one not yet started. A server that could not start
    // contributes none and is not tried again (its error is in describe()); one that ended after it
    // started is started again.
    std::vector<McpTool> tools(const std::function<bool()> & stopped = nullptr);
    // Calls `tool` of `server`: true with the text of the result's content, or false with the error
    // (the tool's own isError text, a refusal, a time limit). `stopped` ends the server's program,
    // which starts again at its next call.
    bool call(const std::string & server,
              const std::string & tool,
              const json & arguments,
              std::string & text,
              const std::function<bool()> & stopped = nullptr);
    // [{name, command, state: "running" | "stopped" | "failed", error?, tools: [{name, description}]}],
    // starting the servers not started yet, for GET /api/agent.
    json describe();
    // Ends every server's program; none starts again.
    void shutdown();

private:
    struct Server {
        McpServerConfig config;
        std::mutex m; // one request at a time per server, and the fields below
        std::unique_ptr<JsonRpcConnection> conn;
        bool failed = false;
        std::string error;
        std::vector<McpTool> tools;
        // A copy of `tools` for tools() to read while a call holds `m`: a round must not wait for
        // another conversation's call to learn what it may offer.
        std::mutex listed_m;
        std::vector<McpTool> listed;
    };
    // The server running and initialised, under its lock.
    bool ensure(Server & s, const std::function<bool()> & stopped);
    Server * find(const std::string & name);

    std::vector<std::unique_ptr<Server>> servers_;
    StartFn start_;
    std::atomic<bool> shut_{false};
};

} // namespace bmoe::server
