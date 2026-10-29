// Who may reach the API, decided from what a request shows and nothing else: no HTTP library, no
// session, so the rules are tested on their own (tests/server_test.cpp) and every transport that
// fronts the engine applies the same ones.
//
// The server is a local program for one person, and it is protected as one: against the browser
// (other pages open on the same machine) by the host and origin rules, and, once it listens beyond
// loopback, against the network by a key. docs/server-api.md, "Security model", is the statement
// these rules implement.
#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bmoe::server {

// What the operator set at launch.
struct AccessPolicy {
    bool loopback = true;          // bound to a loopback address: only loopback host names are answered
    std::set<std::string> origins; // browser origins the API answers to: the UI's own, and --allow-origin
    std::string api_key;           // required as a bearer token on every API request; empty means none
};

// What one request shows. Headers are passed as received; an absent one is "".
struct RequestFacts {
    std::string method;
    std::string path;
    std::string host; // the Host header, port and all
    bool has_origin = false;
    std::string origin; // the Origin header, when has_origin
    std::string sec_fetch_site;
    std::string content_type;
    std::string authorization;
    std::string x_api_key;   // the X-Api-Key header: how Anthropic clients send a key
    std::string remote_addr; // the connection's peer address, which a client cannot choose
};

// status 0 when the request may proceed; otherwise the status to answer with, and why.
struct AccessDecision {
    int status = 0;
    std::string error;
    std::string code; // what a client decides on, when the refusal has one ("local_only")
    bool allowed() const { return status == 0; }
};

// /api/* and /v1/*: the engine. Everything else is the UI's static files.
bool is_api_path(const std::string & path);

// "127.0.0.1", "localhost", "::1" and "[::1]", with or without a port.
bool is_loopback_host(const std::string & host);

// The address a person opens for a server bound to `bind_host`: a wildcard bind ("0.0.0.0", "::")
// is reached through loopback, and an IPv6 literal is bracketed, as a URL needs it to be.
std::string browse_url(const std::string & bind_host, int port);

// `authorization` is "Bearer <key>" (the scheme case-insensitive, as RFC 6750 has it), compared to
// `key` in constant time. An empty key matches nothing.
bool bearer_matches(const std::string & authorization, const std::string & key);

// `token` is `key`, compared in constant time. An empty key matches nothing.
bool key_matches(const std::string & token, const std::string & key);

// A route only the person at this machine may use, whatever the server's --host and key: it
// installs software, reads the network on the person's behalf, or changes files. `path` is matched
// segment by segment: `*` is any one segment, a final `**` any number of them.
struct LocalOnlyRoute {
    const char * method; // "GET", "POST", ...; "*" for any
    const char * path;
    const char * what; // finishes "only a client on this machine may ..."
};

// Every such route. The one list: a route is made local-only by adding a row here, and the guard
// in front of every request refuses it from the network (403, code "local_only") before routing.
const std::vector<LocalOnlyRoute> & local_only_routes();

// The row of local_only_routes() that `method` and `path` fall under, or nullptr.
const LocalOnlyRoute * local_only_route(const std::string & method, const std::string & path);

AccessDecision check_access(const AccessPolicy & policy, const RequestFacts & req);

// Why the server may not start with these launch options, or "" when it may. Listening beyond
// loopback without a key is refused unless asked for by name (`no_api_key`).
std::string launch_refusal(const std::string & bind_host, const std::string & api_key, bool no_api_key);

// The key held in the file at `path` (--api-key-file): its contents without trailing line breaks
// and blanks, so `echo KEY > file` works. A key on the command line is visible to every local user
// through the process list; a file can be readable by its owner alone. False, with the reason, when
// the file cannot be read or holds no key.
bool read_api_key_file(const std::string & path, std::string & key, std::string & err);

// The peer address of a connection is this machine: 127.0.0.0/8, ::1, or an IPv4-mapped 127.x.
// Unlike is_loopback_host() this is the socket's own address, which a client cannot choose.
bool is_loopback_peer(const std::string & remote_addr);

// The agent's routes that reach this computer's files, processes or secrets answer only to a client
// whose connection comes from this machine, whatever --host says (docs/agent.md, "Who may use it").
// `Always`: the route itself does (a workspace added, an approval, a skill, a task, a provider, a
// client's settings written). `IfConversationReaches`: it does when the conversation it names has a
// workspace or sends to a saved provider, which only the handler can look up. The apps reach the
// server on loopback, so every route stays theirs.
enum class Locality { Anywhere, Always, IfConversationReaches };
Locality agent_route_locality(const std::string & path);

// The code a local-only refusal carries (403).
inline constexpr const char * k_local_only_code = "local_only";

// The model path a client named, as the path the engine will open: canonical, symbolic links and
// `..` resolved, and inside one of `roots` - the folders whose models the server lists. A relative
// path is taken relative to the first root, the models folder. False, with the reason, for a path
// that resolves anywhere else, so a confined client cannot have the server open another file
// through `../` or a link. An empty path (no model) is returned as it is.
bool confine_model_path(const std::string & requested,
                        const std::vector<std::string> & roots,
                        std::string & out,
                        std::string & err);

// Headers every response carries, the UI's files above all: what a browser may do with what this
// server sends. The page loads its script, styles, fonts and images from this origin and nothing
// else (an image in model output cannot be fetched from another host), runs no inline script,
// cannot be framed, sends no referrer, and has no content type guessed for it.
const std::vector<std::pair<std::string, std::string>> & security_headers();

// The preview of a workspace file (`GET /api/conversations/<id>/file`) is framed by the UI and may
// be a page with script of its own: it gets these instead, a policy that sandboxes it and lets it
// reach nothing. `is_preview_path` says which responses those are.
bool is_preview_path(const std::string & path);
const std::vector<std::pair<std::string, std::string>> & preview_headers();

} // namespace bmoe::server
