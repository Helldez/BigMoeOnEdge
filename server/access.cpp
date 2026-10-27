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

std::string browse_url(const std::string & bind_host, int port) {
    std::string h = bind_host;
    if (h.empty() || h == "0.0.0.0") h = "127.0.0.1";
    if (h == "::" || h == "[::]") h = "::1";
    if (h.find(':') != std::string::npos && h.front() != '[') h = "[" + h + "]";
    return "http://" + h + ":" + std::to_string(port) + "/";
}

bool bearer_matches(const std::string & authorization, const std::string & key) {
    static const std::string scheme = "Bearer ";
    if (key.empty() || authorization.size() < scheme.size()) return false;
    for (size_t i = 0; i < scheme.size(); ++i)
        if (std::tolower((unsigned char) authorization[i]) != std::tolower((unsigned char) scheme[i])) return false;
    return key_matches(authorization.substr(scheme.size()), key);
}

bool key_matches(const std::string & token, const std::string & key) {
    if (key.empty()) return false;
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
        return {403, "this server only answers to loopback host names", ""};

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
        return {403, "cross-origin request refused (start the server with --allow-origin to permit one)", ""};

    // The key, where one is set, gates the engine and not the UI's files: a page is nothing without
    // the API behind it, and a client on the network has no page.
    // Either header carries it: Bearer is how OpenAI clients and the UI send a key, X-Api-Key how
    // Anthropic clients do.
    if (api && !policy.api_key.empty() && !bearer_matches(r.authorization, policy.api_key) &&
        !key_matches(r.x_api_key, policy.api_key))
        return {401, "an API key is required: send it as Authorization: Bearer <key> (or X-Api-Key: <key>)", ""};

    // The UI's origin is trusted only as far as its script: the page also shows model output, and
    // markup in it is same-origin. A form can post text that parses as JSON but cannot name the
    // type, so every write must.
    if (api && write && !is_json_content_type(r.content_type))
        return {415, "requests that change something must be sent as Content-Type: application/json", ""};

    // Last: a client on the network that passed every rule above, key included, is still not the
    // person at this machine.
    if (const LocalOnlyRoute * l = local_only_route(r.method, r.path))
        if (!is_loopback_peer(r.remote_addr))
            return {403, std::string("only a client on this machine may ") + l->what, k_local_only_code};

    return {};
}

const std::vector<LocalOnlyRoute> & local_only_routes() {
    static const std::vector<LocalOnlyRoute> routes = {
        {"DELETE", "/api/models/local/**", "delete a model"},
        {"POST", "/api/models/url", "add a model from a link"},
        {"GET", "/api/backends", "list the backend packs"},
        {"POST", "/api/backends/*/install", "install a backend pack"},
        {"GET", "/api/update", "check for an update"},
        {"POST", "/api/update/download", "download an update"},
    };
    return routes;
}

namespace {

std::vector<std::string> path_segments(const std::string & path) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= path.size()) {
        const size_t j = path.find('/', i);
        const size_t end = j == std::string::npos ? path.size() : j;
        if (end > i) out.push_back(path.substr(i, end - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return out;
}

bool pattern_matches(const std::string & pattern, const std::string & path) {
    const std::vector<std::string> p = path_segments(pattern);
    const std::vector<std::string> s = path_segments(path);
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == "**") return s.size() > i; // at least one segment below the prefix
        if (i >= s.size()) return false;
        if (p[i] != "*" && p[i] != s[i]) return false;
    }
    return p.size() == s.size();
}

} // namespace

const LocalOnlyRoute * local_only_route(const std::string & method, const std::string & path) {
    // HEAD is answered by a GET route's handler, so it is held to that route's rule.
    const std::string m = method == "HEAD" ? "GET" : method;
    for (const LocalOnlyRoute & r : local_only_routes())
        if ((std::string(r.method) == "*" || m == r.method) && pattern_matches(r.path, path)) return &r;
    return nullptr;
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

Locality agent_route_locality(const std::string & method, const std::string & path) {
    const auto under = [&](const char * prefix) {
        const std::string p = prefix;
        return path == p || path.compare(0, p.size() + 1, p + "/") == 0;
    };
    const bool read = method == "GET" || method == "HEAD";
    if (under("/api/workspaces") || under("/api/skills") || under("/api/tasks") || under("/api/providers") ||
        under("/api/connect"))
        return read ? Locality::Anywhere : Locality::Always;
    static const std::string conv = "/api/conversations/";
    if (path.compare(0, conv.size(), conv) == 0) {
        const size_t slash = path.find('/', conv.size());
        const std::string tail = slash == std::string::npos ? "" : path.substr(slash + 1);
        if (tail == "approval") return Locality::Always;
        if (tail == "messages" || tail == "edit" || tail == "file") return Locality::IfConversationReaches;
    }
    return Locality::Anywhere;
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
        {"Content-Security-Policy",
         "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
         "font-src 'self'; connect-src 'self'; media-src 'none'; object-src 'none'; "
         "frame-src 'self' blob:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'"},
        {"X-Content-Type-Options", "nosniff"},
        {"Referrer-Policy", "no-referrer"},
        {"X-Frame-Options", "DENY"},
    };
    return h;
}

bool is_preview_path(const std::string & path) {
    // /api/conversations/<id>/file, with exactly one segment for the id.
    static const std::string head = "/api/conversations/";
    static const std::string tail = "/file";
    if (path.size() <= head.size() + tail.size()) return false;
    if (path.compare(0, head.size(), head) != 0) return false;
    if (path.compare(path.size() - tail.size(), tail.size(), tail) != 0) return false;
    const std::string id = path.substr(head.size(), path.size() - head.size() - tail.size());
    return !id.empty() && id.find('/') == std::string::npos;
}

const std::vector<std::pair<std::string, std::string>> & preview_headers() {
    // A workspace file is the agent's work, not this UI: an HTML page or an SVG it wrote may carry
    // script. The policy itself sandboxes the document (an opaque origin, whatever frame shows it),
    // lets its own inline script and styles run so a page the agent built can be tried, and lets it
    // load nothing from anywhere: no request to this server's API, no other host, no form. Only the
    // UI's own page may frame it.
    static const std::vector<std::pair<std::string, std::string>> h = {
        {"Content-Security-Policy", "sandbox allow-scripts; default-src 'none'; script-src 'unsafe-inline'; "
                                    "style-src 'unsafe-inline'; img-src data: blob:; font-src data:; "
                                    "media-src data: blob:; connect-src 'none'; object-src 'none'; base-uri 'none'; "
                                    "form-action 'none'; frame-ancestors 'self'"},
        {"X-Content-Type-Options", "nosniff"},
        {"Referrer-Policy", "no-referrer"},
        {"X-Frame-Options", "SAMEORIGIN"},
    };
    return h;
}

} // namespace bmoe::server
