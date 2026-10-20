#include "access.h"

#include "json_util.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

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
    // the machine or hold a worker, and a page can issue one with an image tag. A cross-origin write
    // carries Origin; a cross-origin read carries none and is told apart by Sec-Fetch-Site, which the
    // browser sets and a page cannot. A client that is not a browser sends neither, and is the local
    // program this server is for.
    const bool api = is_api_path(r.path);
    const bool write = r.method != "GET" && r.method != "HEAD";
    // Sec-Fetch-Site is trusted only for the two answers that mean "this page" or "the person":
    // `same-site` is another port of the same host, which is exactly where a hostile local page
    // lives, and a value this code does not know is no reason to let a request through.
    const bool fetched_from_elsewhere =
        !r.sec_fetch_site.empty() && r.sec_fetch_site != "same-origin" && r.sec_fetch_site != "none";
    const bool foreign = r.has_origin ? policy.origins.count(r.origin) == 0 : fetched_from_elsewhere;
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

bool read_api_key_file(const std::string & path, std::string & key, std::string & err) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        err = "cannot read the API key file " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    if (s.empty()) {
        err = "the API key file " + path + " is empty: a server with an empty key would have no key";
        return false;
    }
    key = std::move(s);
    return true;
}

bool is_loopback_peer(const std::string & remote_addr) {
    static const std::string mapped = "::ffff:";
    const std::string a =
        remote_addr.compare(0, mapped.size(), mapped) == 0 ? remote_addr.substr(mapped.size()) : remote_addr;
    return a == "::1" || a.compare(0, 4, "127.") == 0;
}

bool confine_model_path(const std::string & requested,
                        const std::vector<std::string> & roots,
                        std::string & out,
                        std::string & err) {
    namespace fs = std::filesystem;
    if (requested.empty()) {
        out.clear();
        return true;
    }
    std::vector<fs::path> real_roots;
    for (const std::string & r : roots) {
        if (r.empty()) continue;
        std::error_code ec;
        const fs::path root = fs::weakly_canonical(fs::absolute(fs::u8path(r), ec), ec);
        if (!ec) real_roots.push_back(root);
    }
    if (real_roots.empty() || roots.empty() || roots.front().empty()) {
        err = "there is no models folder to open a model from";
        return false;
    }
    fs::path p = fs::u8path(requested);
    if (p.is_relative()) p = real_roots.front() / p;
    // weakly_canonical resolves every link and `..` along the part of the path that exists, so
    // what is compared is where the file really is, not how the request spelled it.
    std::error_code ec;
    const fs::path real = fs::weakly_canonical(p, ec);
    if (!ec) {
        for (const fs::path & root : real_roots) {
            const fs::path rel = real.lexically_relative(root);
            if (!rel.empty() && rel != "." && *rel.begin() != "..") {
                out = real.u8string();
                return true;
            }
        }
    }
    err = "a model must be a file in a folder the server lists models from (" + real_roots.front().u8string() + ")";
    return false;
}

const std::vector<std::pair<std::string, std::string>> & security_headers() {
    // Nothing inline at all: the built UI is one script and one stylesheet from this origin and sets
    // no style attribute, so neither needs 'unsafe-inline'. A UI change that inlines one breaks
    // visibly in the browser console, which is where it should be noticed. form-action 'none' costs
    // nothing: the UI's forms are handled in script and never submitted.
    static const std::vector<std::pair<std::string, std::string>> h = {
        {"Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
                                    "font-src 'self'; connect-src 'self'; media-src 'none'; object-src 'none'; "
                                    "base-uri 'none'; form-action 'none'; frame-ancestors 'none'"},
        {"X-Content-Type-Options", "nosniff"},
        {"Referrer-Policy", "no-referrer"},
        {"X-Frame-Options", "DENY"},
    };
    return h;
}

} // namespace bmoe::server
