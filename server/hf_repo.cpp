#include "hf_repo.h"

#include "model_files.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace bmoe::server {

namespace {

bool segment_char(char c) {
    return std::isalnum((unsigned char) c) || c == '.' || c == '_' || c == '-' || c == '+';
}

// One path segment as a file name on disk and in a URL: nothing that escapes a folder or needs
// escaping. "." and ".." are segments that name a folder, not a file, and are refused with the
// empty one.
bool valid_segment(const std::string & s) {
    if (s.empty() || s == "." || s == "..") return false;
    return std::all_of(s.begin(), s.end(), segment_char);
}

bool valid_repo(const std::string & repo) {
    const size_t slash = repo.find('/');
    if (slash == std::string::npos || repo.find('/', slash + 1) != std::string::npos) return false;
    const std::string owner = repo.substr(0, slash), name = repo.substr(slash + 1);
    return valid_segment(owner) && valid_segment(name) && owner[0] != '.' && name[0] != '.';
}

bool valid_path(const std::string & path) {
    if (path.empty() || path.front() == '/' || path.back() == '/') return false;
    size_t i = 0;
    while (i <= path.size()) {
        const size_t j = path.find('/', i);
        const std::string seg = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!valid_segment(seg)) return false;
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return true;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char) s.back()))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char) s[i]))
        ++i;
    return s.substr(i);
}

bool strip_prefix(std::string & s, const char * prefix) {
    const size_t n = std::char_traits<char>::length(prefix);
    if (s.compare(0, n, prefix) != 0) return false;
    s.erase(0, n);
    return true;
}

std::vector<std::string> split(const std::string & s, char sep) {
    std::vector<std::string> out;
    size_t i = 0;
    while (true) {
        const size_t j = s.find(sep, i);
        out.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return out;
}

std::string lower(std::string s) {
    for (char & c : s)
        c = (char) std::tolower((unsigned char) c);
    return s;
}

std::string base_name(const std::string & path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string dir_name(const std::string & path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

std::string mib(uint64_t b) {
    return std::to_string(b >> 20) + " MiB";
}

} // namespace

bool parse_hf_ref(const std::string & text, HfRef & out, std::string & error) {
    std::string s = trim(text);
    const size_t q = s.find_first_of("?#");
    if (q != std::string::npos) s.erase(q);
    strip_prefix(s, "https://") || strip_prefix(s, "http://");
    if (strip_prefix(s, "huggingface.co/") || strip_prefix(s, "www.huggingface.co/") || strip_prefix(s, "hf.co/")) {
        // the site's own prefixes for a model page are not part of the id
        strip_prefix(s, "models/");
    } else if (s.find("://") != std::string::npos || s.find('.') < s.find('/')) {
        // another host, or a host-looking first segment: not a repository id and not this site
        error = "not a Hugging Face repository id or URL: " + trim(text);
        return false;
    }
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    std::vector<std::string> parts = split(s, '/');
    if (parts.size() < 2) {
        error = "a repository id is owner/name";
        return false;
    }
    HfRef r;
    r.repo = parts[0] + "/" + parts[1];
    if (!valid_repo(r.repo)) {
        error = "a repository id is owner/name, each of letters, digits, '.', '_' or '-'";
        return false;
    }
    if (parts.size() > 2) {
        // /tree/<rev>[/<folder>], /blob/<rev>/<path>, /resolve/<rev>/<path>
        const std::string & kind = parts[2];
        if ((kind != "tree" && kind != "blob" && kind != "resolve") || parts.size() < 4) {
            error = "after the repository, a URL names /tree, /blob or /resolve and a revision";
            return false;
        }
        r.revision = parts[3];
        if (!valid_segment(r.revision)) {
            error = "the revision is not a branch, tag or commit name";
            return false;
        }
        if (parts.size() > 4) {
            std::string path = parts[4];
            for (size_t i = 5; i < parts.size(); ++i)
                path += "/" + parts[i];
            if (!valid_path(path)) {
                error = "the path inside the repository has a segment this server does not accept";
                return false;
            }
            if (kind != "tree") r.file = path;
        }
    }
    out = r;
    return true;
}

namespace {

// A token that reads as a quantization: Q4_K_M, IQ2_M, Q8_0, BF16, F16, F32, MXFP4.
bool quant_token(const std::string & up) {
    if (up == "BF16" || up == "F16" || up == "F32" || up == "MXFP4" || up == "NVFP4") return true;
    size_t i = 0;
    if (up.compare(0, 2, "IQ") == 0)
        i = 2;
    else if (up.compare(0, 1, "Q") == 0)
        i = 1;
    else
        return false;
    if (i >= up.size() || !std::isdigit((unsigned char) up[i])) return false;
    return std::all_of(up.begin() + (long) i, up.end(),
                       [](char c) { return std::isalnum((unsigned char) c) || c == '_'; });
}

} // namespace

std::string quant_from_name(const std::string & name) {
    // The last '-' token of the stem (shard suffix and extension off) that reads as a
    // quantization, with an unsloth "UD-" prefix kept when it precedes one.
    std::string stem = base_name(name);
    ShardName sh;
    if (parse_shard_name(stem, sh))
        stem = sh.stem;
    else if (lower(stem).size() > 5 && lower(stem).compare(stem.size() - 5, 5, ".gguf") == 0)
        stem.erase(stem.size() - 5);
    std::string up = stem;
    for (char & c : up)
        c = (char) std::toupper((unsigned char) c);
    const std::vector<std::string> toks = split(up, '-');
    for (size_t i = toks.size(); i-- > 0;) {
        if (!quant_token(toks[i])) continue;
        return (i > 0 && toks[i - 1] == "UD" ? "UD-" : "") + toks[i];
    }
    return "";
}

bool parse_hf_tree(const json & tree, std::vector<HfModel> & out, std::string & error) {
    out.clear();
    if (!tree.is_array()) {
        error = tree.is_object() && tree.contains("error") && tree["error"].is_string()
                    ? tree["error"].get<std::string>()
                    : "the listing is not an array of files";
        return false;
    }
    // Shards group by folder and stem, as model_files.h groups a directory; a set's order is its index.
    struct Group {
        std::string first; // path of shard 1, or the lone file
        int count = 1;
        std::map<int, HfFile> shards;
        HfFile lone;
        bool split = false;
    };
    std::map<std::string, Group> groups;
    for (const json & e : tree) {
        if (!e.is_object() || e.value("type", "") != "file" || !e.contains("path") || !e["path"].is_string()) continue;
        const std::string path = e["path"].get<std::string>();
        if (!is_gguf_name(base_name(path)) || !valid_path(path)) continue;
        HfFile f;
        f.path = path;
        if (e.contains("size") && e["size"].is_number_unsigned()) f.bytes = e["size"].get<uint64_t>();
        if (f.bytes == 0 && e.contains("lfs") && e["lfs"].is_object() && e["lfs"].value("size", (uint64_t) 0))
            f.bytes = e["lfs"]["size"].get<uint64_t>();
        ShardName sh;
        if (parse_shard_name(base_name(path), sh)) {
            Group & g = groups[dir_name(path) + sh.stem + "#split"];
            g.split = true;
            g.count = sh.count;
            g.first = dir_name(path) + shard_file_name(sh.stem, 1, sh.count);
            g.shards[sh.index] = f;
        } else {
            Group & g = groups[path];
            g.first = path;
            g.lone = f;
        }
    }
    for (auto & kv : groups) {
        Group & g = kv.second;
        HfModel m;
        m.name = g.first;
        m.count = g.count;
        if (g.split) {
            for (auto & s : g.shards)
                m.files.push_back(s.second);
        } else {
            m.files.push_back(g.lone);
        }
        for (const HfFile & f : m.files)
            m.bytes += f.bytes;
        m.quant = quant_from_name(m.name);
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const HfModel & a, const HfModel & b) { return a.name < b.name; });
    return true;
}

namespace {

// RFC 3986: the unreserved characters as they are, every other byte as %XX.
std::string percent_encode(const std::string & text) {
    static const char * hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += (char) c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

} // namespace

std::string hf_search_url(const std::string & endpoint, const std::string & query, int limit) {
    return endpoint + "/api/models?search=" + percent_encode(query) +
           "&filter=gguf&sort=downloads&direction=-1&limit=" + std::to_string(limit);
}

bool parse_hf_search(const json & hits, std::vector<HfRepoHit> & out, std::string & error) {
    out.clear();
    if (!hits.is_array()) {
        error = hits.is_object() && hits.contains("error") && hits["error"].is_string()
                    ? hits["error"].get<std::string>()
                    : "the search answer is not a list";
        return false;
    }
    for (const json & h : hits) {
        if (!h.is_object() || !h.contains("id") || !h["id"].is_string()) continue;
        HfRepoHit r;
        r.id = h["id"].get<std::string>();
        if (!valid_repo(r.id)) continue; // a name this server could not list anyway
        if (h.contains("downloads") && h["downloads"].is_number_unsigned())
            r.downloads = h["downloads"].get<uint64_t>();
        if (h.contains("likes") && h["likes"].is_number_unsigned()) r.likes = h["likes"].get<uint64_t>();
        if (h.contains("lastModified") && h["lastModified"].is_string())
            r.modified = h["lastModified"].get<std::string>();
        // The site says "auto", "manual" or false; anything that is not false or absent gates.
        if (h.contains("gated"))
            r.gated = !(h["gated"].is_boolean() && !h["gated"].get<bool>()) && !h["gated"].is_null();
        out.push_back(std::move(r));
    }
    return true;
}

std::string hf_tree_url(const std::string & endpoint, const HfRef & ref) {
    return endpoint + "/api/models/" + ref.repo + "/tree/" + ref.revision + "?recursive=true";
}

std::string hf_resolve_url(const std::string & endpoint, const HfRef & ref, const std::string & path) {
    return endpoint + "/" + ref.repo + "/resolve/" + ref.revision + "/" + path + "?download=true";
}

bool is_hf_catalog_id(const std::string & id) {
    return id.compare(0, 3, "hf:") == 0;
}

CatalogEntry hf_catalog_entry(const std::string & endpoint, const HfRef & ref, const HfModel & model) {
    CatalogEntry e;
    e.id = "hf:" + ref.repo + "/" + model.name;
    std::string title = base_name(model.name);
    ShardName sh;
    if (parse_shard_name(title, sh))
        title = sh.stem;
    else if (title.size() > 5)
        title.erase(title.size() - 5); // ".gguf"
    e.title = title;
    e.quant = model.quant;
    e.blurb = "From Hugging Face: " + ref.repo + (ref.revision == "main" ? "" : " at " + ref.revision);
    for (const HfFile & f : model.files)
        e.files.push_back({base_name(f.path), hf_resolve_url(endpoint, ref, f.path), f.bytes});
    return e;
}

FitVerdict fit_verdict(const FitInputs & in) {
    FitVerdict v;
    const uint64_t needed = in.file_bytes > in.present_bytes ? in.file_bytes - in.present_bytes : 0;
    if (in.disk_free == 0) {
        v.disk = "free space in the models folder could not be read";
    } else if (needed > in.disk_free) {
        v.downloadable = false;
        v.disk = "needs " + mib(needed) + " of disk and the models folder has " + mib(in.disk_free) +
                 " free: " + mib(needed - in.disk_free) + " short";
    } else {
        v.disk = "needs " + mib(needed) + " of disk" +
                 (in.present_bytes ? " (" + mib(in.present_bytes) + " already here)" : "") +
                 "; the models folder has " + mib(in.disk_free) + " free";
    }
    if (!in.readable) {
        v.mode = "not readable";
        v.memory = "the file's header could not be read, so nothing about it is known";
        return v;
    }
    if (!in.is_moe) {
        v.mode = "plain mmap";
        v.memory =
            "a dense model: this engine runs it whole from the file, nothing is streamed" +
            (in.budget ? std::string("; it is ") + mib(in.file_bytes) + " against " + mib(in.budget) +
                             " this process may hold, " +
                             (in.file_bytes <= in.budget
                                  ? "so it fits in memory"
                                  : "so the parts the system cannot hold are read back from the file as they are used")
                       : std::string());
        return v;
    }
    if (!in.streamable) {
        v.mode = "plain mmap";
        v.memory = "a MoE model whose experts this engine does not stream" +
                   (in.refusal.empty() ? std::string() : ": " + in.refusal) + "; it runs whole from the file";
        return v;
    }
    v.mode = "streams";
    v.memory = "the experts stream from flash and " + mib(in.dense_bytes) +
               " stays resident, plus a cache of at least one token cycle (" + mib(in.token_cycle) + ")";
    if (in.budget) {
        const uint64_t floor = in.dense_bytes + in.token_cycle;
        v.memory += floor <= in.budget ? "; that fits in the " + mib(in.budget) +
                                             " this process may hold, and the plan sizes the cache from the rest"
                                       : "; that is more than the " + mib(in.budget) +
                                             " this process may hold, so the plan will decline or run it slowly";
    }
    return v;
}

} // namespace bmoe::server
