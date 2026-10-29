// The agent's HTTP routes (docs/agent.md, docs/server-api.md#agent): conversations and their runs,
// workspaces, skills, tasks, providers and connecting other apps. Registered by http_server.cpp
// beside the rest of the API; the policy they call lives in agent.h, agent_store.h and connect.h.
#pragma once

#include "agent.h"
#include "agent_store.h"
#include "engine_host.h"
#include "json_util.h"

#include <functional>
#include <string>

namespace httplib {
class Server;
}

namespace bmoe::server {

struct AgentRouteDeps {
    agent::Agent * agent = nullptr;
    agent::AgentStore * store = nullptr;
    EngineHost * host = nullptr;
    EventBus * bus = nullptr;
    std::string user_skills_dir; // `<data>/skills`: where a person's own skills are written
    json clients;                // catalog/clients.json, parsed ({} when there is none)
    std::string origin;          // the address clients reach this server at, for their settings
    bool has_key = false;        // the server requires an API key
    std::string home;            // the user's home folder (platform::home_dir())
};

// `deps` must outlive the server.
void register_agent_routes(httplib::Server & svr, const AgentRouteDeps & deps);

} // namespace bmoe::server
