#include "access.h"

#include "json_util.h"

#include <cctype>

namespace bmoe::server {

bool is_api_path(const std::string & path) {
    return path.compare(0, 5, "/api/") == 0 || path.compare(0, 4, "/v1/") == 0;
}

// The port goes: after the bracket for a bracketed IPv6 literal, after the colon when there is
// one. A bare IPv6 literal ("::1", as --host takes it) has several colons and no port.
static std::string without_port(const std::string & host) {
    const size_t bracket = host.rfind(']');
    if (bracket != std::string::npos) return host.substr(0, bracket + 1);
    const size_t colon = host.find(':');
    if (colon != std::string::npos && colon == host.rfind(':')) return host.substr(0, colon);
    return host;
}

bool is_loopback_host(const std::string & host) {
    const std::string h = without_port(host);
    return h == "127.0.0.1" || h == "localhost" || h == "::1" || h == "[::1]";
}

bool bearer_matches(const std::string & authorization, const std::string & key) {
    static const std::string scheme = "Bearer ";
    if (key.empty() || authorization.size() < scheme.size()) return false;
    for (size_t i = 0; i < scheme.size(); ++i)
        if (std::tolower((unsigned char) authorization[i]) != std::tolower((unsigned char) scheme[i])) return false;
    const std::string token = authorization.substr(scheme.size());
    // Every byte of the key is compared whatever the first mismatch, and a token of the wrong
    // length fails after the loop like any other: the time taken says nothing about the key.
    unsigned diff = static_cast<unsigned>(token.size() ^ key.size());
    for (size_t i = 0; i < key.size(); ++i) {
        const unsigned k = static_cast<unsigned char>(key[i]);
        const unsigned t = i < token.size() ? static_cast<unsigned char>(token[i]) : 0u;
        diff |= k ^ t;
    }
    return diff == 0;
}

AccessDecision check_access(const AccessPolicy & policy, const RequestFacts & r) {
    // A loopback server answers only to loopback names: a DNS-rebinding page cannot reach it
    // through a host name of its own.
    if (policy.loopback && !r.host.empty() && !is_loopback_host(r.host))
        return {403, "this server only answers to loopback host names"};

    // A browser request must come from the UI's own origin, on every method: a GET here can measure
    // the machine or hold a worker, and a page can issue one with an image tag. A cross-site write
    // carries Origin; a cross-site read carries none and is told apart by Sec-Fetch-Site, which the
    // browser sets and a page cannot. A client that is not a browser sends neither, and is the local
    // program this server is for.
    const bool api = is_api_path(r.path);
    const bool write = r.method != "GET" && r.method != "HEAD";
    const bool foreign = r.has_origin ? policy.origins.count(r.origin) == 0 : r.sec_fetch_site == "cross-site";
    if ((api || write) && foreign)
        return {403, "cross-origin request refused (start the server with --allow-origin to permit one)"};

    // The key, where one is set, gates the engine and not the UI's files: a page is nothing without
    // the API behind it, and a client on the network has no page.
    if (api && !policy.api_key.empty() && !bearer_matches(r.authorization, policy.api_key))
        return {401, "an API key is required: send it as Authorization: Bearer <key>"};

    // The UI's origin is trusted only as far as its script: the page also shows model output, and
    // markup in it is same-origin. A form can post text that parses as JSON but cannot name the
    // type, so every write must.
    if (api && write && !is_json_content_type(r.content_type))
        return {415, "requests that change something must be sent as Content-Type: application/json"};

    return {};
}

std::string launch_refusal(const std::string & bind_host, const std::string & api_key, bool no_api_key) {
    if (is_loopback_host(bind_host) || !api_key.empty() || no_api_key) return "";
    return "--host " + bind_host +
           " would let anyone who can reach this address use the engine. Set an API key (--api-key KEY; "
           "clients send it as Authorization: Bearer KEY), or pass --no-api-key to listen open on a network "
           "you trust.";
}

} // namespace bmoe::server
