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
};

// status 0 when the request may proceed; otherwise the status to answer with, and why.
struct AccessDecision {
    int status = 0;
    std::string error;
    bool allowed() const { return status == 0; }
};

// /api/* and /v1/*: the engine. Everything else is the UI's static files.
bool is_api_path(const std::string & path);

// "127.0.0.1", "localhost", "::1" and "[::1]", with or without a port.
bool is_loopback_host(const std::string & host);

// `authorization` is "Bearer <key>" (the scheme case-insensitive, as RFC 6750 has it), compared to
// `key` in constant time. An empty key matches nothing.
bool bearer_matches(const std::string & authorization, const std::string & key);

AccessDecision check_access(const AccessPolicy & policy, const RequestFacts & req);

// Why the server may not start with these launch options, or "" when it may. Listening beyond
// loopback without a key is refused unless asked for by name (`no_api_key`).
std::string launch_refusal(const std::string & bind_host, const std::string & api_key, bool no_api_key);

} // namespace bmoe::server
