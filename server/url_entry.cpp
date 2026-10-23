#include "url_entry.h"

#include "hf_repo.h"
#include "model_files.h"

#include <cctype>

namespace bmoe::server {

namespace {

constexpr const char * k_scheme = "https://";
constexpr const char * k_id_prefix = "url:";
// Longer than any file name a filesystem takes once ".part" is added to it.
constexpr size_t k_max_name = 200;

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool percent_decode(const std::string & s, std::string & out) {
    out.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        if (i + 2 >= s.size()) return false;
        const int hi = hex_value(s[i + 1]), lo = hex_value(s[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out += (char) (hi * 16 + lo);
        i += 2;
    }
    return true;
}

// A file name the models folder holds as it is, and the same alphabet a repository's names are
// held to (hf_repo.cpp): nothing that escapes a folder, needs quoting or hides the file.
bool plain_model_name(const std::string & s) {
    if (s.empty() || s.size() > k_max_name || s[0] == '.' || s[0] == '-' || !is_gguf_name(s)) return false;
    for (char c : s)
        if (!(std::isalnum((unsigned char) c) || c == '.' || c == '_' || c == '-' || c == '+')) return false;
    return true;
}

// Characters a URL never carries raw: controls, space, and the ones RFC 3986 leaves out.
bool raw_url_char(char c) {
    const unsigned char u = (unsigned char) c;
    if (u <= 0x20 || u >= 0x7f) return false;
    for (char bad : std::string("\"<>\\^`{|}"))
        if (c == bad) return false;
    return true;
}

std::string trim(const std::string & s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char) s[b]))
        ++b;
    while (e > b && std::isspace((unsigned char) s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

} // namespace

bool parse_model_url(const std::string & text, UrlModel & out, std::string & error) {
    out = {};
    std::string url = trim(text);
    const size_t scheme = std::char_traits<char>::length(k_scheme);
    std::string head = url.substr(0, scheme);
    for (char & c : head)
        c = (char) std::tolower((unsigned char) c);
    if (head != k_scheme) {
        error = "a model link must be an https:// URL";
        return false;
    }
    url = k_scheme + url.substr(scheme);
    for (char c : url)
        if (!raw_url_char(c)) {
            error = "the link holds a character a URL cannot carry as it is (a space, a quote, a control)";
            return false;
        }
    const size_t hash = url.find('#');
    if (hash != std::string::npos) url.erase(hash);

    const size_t path_at = url.find_first_of("/?", scheme);
    const std::string authority =
        url.substr(scheme, path_at == std::string::npos ? std::string::npos : path_at - scheme);
    if (authority.empty() || authority.find('@') != std::string::npos) {
        error = "the link needs a host, and no user name or password in it";
        return false;
    }
    if (path_at == std::string::npos || url[path_at] != '/') {
        error = "the link must name a file: it has no path";
        return false;
    }
    const size_t query_at = url.find('?', path_at);
    const std::string path =
        url.substr(path_at, query_at == std::string::npos ? std::string::npos : query_at - path_at);
    const std::string query = query_at == std::string::npos ? "" : url.substr(query_at);
    const size_t last = path.find_last_of('/');
    const std::string prefix = url.substr(0, path_at) + path.substr(0, last + 1);
    const std::string segment = path.substr(last + 1);
    std::string name;
    if (!percent_decode(segment, name) || !plain_model_name(name)) {
        error = "the link must end in a .gguf file name made of letters, digits and ._-+";
        return false;
    }

    out.host = authority;
    ShardName sh;
    if (!parse_shard_name(name, sh) || sh.count == 1) {
        out.files.push_back({name, prefix + segment + query, 0, {}});
        return true;
    }
    // A split set is fetched whole: its other shards sit beside the one linked, named as
    // gguf-split names them, so their links are the same link with the name changed.
    if (sh.count > kMaxUrlShards) {
        error = "the link names a split set of " + std::to_string(sh.count) + " shards; at most " +
                std::to_string(kMaxUrlShards) + " are fetched from a link";
        return false;
    }
    if (segment != name) {
        error = "a link to a shard of a split set must spell its name without escapes";
        return false;
    }
    for (int i = 1; i <= sh.count; ++i) {
        const std::string n = shard_file_name(sh.stem, i, sh.count);
        out.files.push_back({n, prefix + n + query, 0, {}});
    }
    return true;
}

bool fetch_url_sizes(HttpFetch & net,
                     const UrlModel & model,
                     std::chrono::steady_clock::duration deadline,
                     std::vector<uint64_t> & sizes,
                     bool & timed_out,
                     std::string & error,
                     const std::function<std::chrono::steady_clock::time_point()> & now) {
    sizes.clear();
    timed_out = false;
    const auto until = now() + deadline;
    for (const CatalogFile & f : model.files) {
        if (now() >= until) {
            timed_out = true;
            error = "the host took longer than " +
                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(deadline).count()) +
                    " s to give the sizes of " + std::to_string(model.files.size()) + " files";
            return false;
        }
        uint64_t n = 0;
        std::string why;
        if (!net.size(f.url, n, why)) {
            error = "could not reach " + f.name + ": " + why;
            return false;
        }
        sizes.push_back(n);
    }
    return true;
}

CatalogEntry url_catalog_entry(const UrlModel & model, const std::vector<uint64_t> & bytes) {
    CatalogEntry e;
    if (model.files.empty()) return e;
    const std::string & first = model.files.front().name;
    e.id = k_id_prefix + first;
    ShardName sh;
    e.title = parse_shard_name(first, sh) ? sh.stem : first.substr(0, first.size() - 5);
    e.quant = quant_from_name(first);
    e.blurb = "From a link to " + model.host;
    for (size_t i = 0; i < model.files.size(); ++i) {
        CatalogFile f = model.files[i];
        f.bytes = i < bytes.size() ? bytes[i] : 0;
        e.files.push_back(f);
    }
    return e;
}

bool is_url_catalog_id(const std::string & id) {
    return id.compare(0, std::char_traits<char>::length(k_id_prefix), k_id_prefix) == 0;
}

HeadAnswer head_answer(const std::string & headers) {
    static const std::string field = "content-length:";
    HeadAnswer a;
    size_t i = 0;
    while (i < headers.size()) {
        size_t j = headers.find('\n', i);
        if (j == std::string::npos) j = headers.size();
        std::string line = headers.substr(i, j - i);
        i = j + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Each response starts with its status line ("HTTP/2 200", "HTTP/1.1 302 Found"): only the
        // last one is the file's.
        if (line.compare(0, 5, "HTTP/") == 0) {
            a = {};
            const size_t sp = line.find(' ');
            if (sp != std::string::npos)
                for (size_t k = sp + 1; k < line.size() && std::isdigit((unsigned char) line[k]); ++k)
                    a.status = a.status * 10 + (line[k] - '0');
            continue;
        }
        if (line.size() <= field.size()) continue;
        bool match = true;
        for (size_t k = 0; k < field.size() && match; ++k)
            match = std::tolower((unsigned char) line[k]) == field[k];
        if (!match) continue;
        uint64_t v = 0;
        bool digits = false;
        for (size_t k = field.size(); k < line.size(); ++k) {
            const char c = line[k];
            if (c == ' ' || c == '\t') {
                if (digits) break;
                continue;
            }
            if (!std::isdigit((unsigned char) c)) {
                digits = false;
                break;
            }
            v = v * 10 + (uint64_t) (c - '0');
            digits = true;
        }
        if (digits) a.length = v;
    }
    return a;
}

} // namespace bmoe::server
