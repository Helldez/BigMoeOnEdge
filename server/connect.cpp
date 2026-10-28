#include "connect.h"

#include "platform.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace bmoe::server {

namespace {

// The lines that frame what this server wrote into a TOML file: the next write replaces what is
// between them and leaves the rest of the file as the person has it.
constexpr const char * kBlockBegin = "# >>> bmoe-server (written by BigMoeOnEdge; replaced on the next connect)";
constexpr const char * kBlockEnd = "# <<< bmoe-server";

// The model a snippet names when none is loaded: the person fills it in, and a file is not written.
constexpr const char * kNoModel = "<model id from /v1/models>";

std::string escape(const std::string & v) {
    std::string out;
    for (char c : v) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

void replace_all(std::string & s, const std::string & from, const std::string & to) {
    size_t at = 0;
    while ((at = s.find(from, at)) != std::string::npos) {
        s.replace(at, from.size(), to);
        at += to.size();
    }
}

std::string expand_home(const std::string & file, const std::string & home) {
    if (file.compare(0, 2, "~/") != 0) return file;
    if (home.empty()) return "";
    return (fs::u8path(home) / fs::u8path(file.substr(2))).u8string();
}

const json * find_entry(const json & catalog, const std::string & id) {
    if (!catalog.is_object() || !catalog.contains("clients") || !catalog["clients"].is_array()) return nullptr;
    for (const json & c : catalog["clients"])
        if (c.is_object() && c.value("id", "") == id) return &c;
    return nullptr;
}

void merge_into(json & dst, const json & src) {
    for (auto it = src.begin(); it != src.end(); ++it) {
        if (dst.contains(it.key()) && dst[it.key()].is_object() && it.value().is_object())
            merge_into(dst[it.key()], it.value());
        else
            dst[it.key()] = it.value();
    }
}

} // namespace

std::string fill_template(const std::string & text, const ConnectFacts & f, bool quoted) {
    const auto v = [&](const std::string & s) { return quoted ? escape(s) : s; };
    std::string s = text;
    replace_all(s, "{{origin}}", v(f.origin));
    replace_all(s, "{{base_url}}", v(f.origin + "/v1"));
    replace_all(s, "{{model}}", v(f.model.empty() ? kNoModel : f.model));
    // The key itself is never written into a snippet or a file: a client is told where it is.
    replace_all(s, "{{api_key}}", v(f.has_key ? "<this server's API key>" : "local"));
    return s;
}

json connect_list(const json & catalog, const ConnectFacts & facts) {
    json out = json::array();
    if (!catalog.is_object() || !catalog.contains("clients") || !catalog["clients"].is_array()) return out;
    for (const json & c : catalog["clients"]) {
        if (!c.is_object() || c.value("id", "").empty() || c.value("name", "").empty() ||
            c.value("snippet", "").empty())
            continue;
        const std::string file = expand_home(c.value("file", ""), facts.home);
        const std::string write = c.value("write", "");
        out.push_back({{"id", c.value("id", "")},
                       {"name", c.value("name", "")},
                       {"how", fill_template(c.value("how", ""), facts, false)},
                       {"snippet", fill_template(c.value("snippet", ""), facts, false)},
                       {"file", file.empty() ? json(nullptr) : json(file)},
                       {"writable", !file.empty() && (write == "toml_block" || write == "json_merge")}});
    }
    return out;
}

bool merge_settings(const std::string & write,
                    const std::string & existing,
                    const std::string & snippet,
                    std::string & out,
                    std::string & err) {
    if (write == "json_merge") {
        json base = json::object();
        if (!existing.empty()) {
            try {
                base = json::parse(existing);
            } catch (const std::exception &) {
                err = "the file is not valid JSON (comments are not read): add the snippet by hand";
                return false;
            }
            if (!base.is_object()) {
                err = "the file does not hold a JSON object";
                return false;
            }
        }
        json add;
        try {
            add = json::parse(snippet);
        } catch (const std::exception & e) {
            err = std::string("the catalog's snippet is not JSON: ") + e.what();
            return false;
        }
        merge_into(base, add);
        out = base.dump(2) + "\n";
        return true;
    }
    if (write == "toml_block") {
        std::string body = existing;
        const size_t b = body.find(kBlockBegin);
        if (b != std::string::npos) {
            const size_t e = body.find(kBlockEnd, b);
            if (e == std::string::npos) {
                err = "the file has the start of a bmoe-server block but not its end: fix it by hand";
                return false;
            }
            size_t after = e + std::string(kBlockEnd).size();
            if (after < body.size() && body[after] == '\n') ++after;
            body.erase(b, after - b);
        }
        // Tables only, appended at the end: a table there cannot capture a top-level key the person
        // wrote, and a top-level key in the block would land inside the person's last table.
        if (!body.empty() && body.back() != '\n') body += '\n';
        if (!body.empty()) body += '\n';
        std::string block = snippet;
        if (!block.empty() && block.back() != '\n') block += '\n';
        out = body + kBlockBegin + "\n" + block + kBlockEnd + "\n";
        return true;
    }
    err = "this client's settings are not written by the server";
    return false;
}

bool connect_write(const json & catalog,
                   const std::string & id,
                   const ConnectFacts & facts,
                   json & result,
                   int & status,
                   std::string & err) {
    const json * c = find_entry(catalog, id);
    if (!c) {
        status = 404;
        err = "no such client";
        return false;
    }
    const std::string file = expand_home(c->value("file", ""), facts.home);
    const std::string write = c->value("write", "");
    if (file.empty() || (write != "toml_block" && write != "json_merge")) {
        status = 400;
        err = "this client's settings are not a file the server writes: copy the snippet";
        return false;
    }
    if (facts.model.empty()) {
        status = 409;
        err = "load a model first: its id is what the client asks for";
        return false;
    }
    // Two writes at once would back up and rewrite the same file in between each other.
    static std::mutex write_m;
    std::lock_guard<std::mutex> lk(write_m);
    const fs::path p = fs::u8path(file);
    std::error_code ec;
    std::string existing;
    const bool exists = fs::exists(p, ec);
    if (exists) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        existing = ss.str();
    }
    std::string text;
    if (!merge_settings(write, existing, fill_template(c->value("snippet", ""), facts, true), text, err)) {
        status = 409;
        return false;
    }
    std::string backup;
    if (exists) {
        char stamp[32];
        const std::time_t t = std::time(nullptr);
        const std::tm tm = platform::utc_time(t);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        backup = file + ".bmoe-backup-" + stamp;
        fs::copy_file(p, fs::u8path(backup), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            status = 500;
            err = "could not back up " + file + ": " + ec.message();
            return false;
        }
    }
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = fs::u8path(file + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            status = 500;
            err = "could not write " + tmp.u8string();
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        status = 500;
        err = "could not replace " + file;
        return false;
    }
    result = json{{"file", file}, {"backup", backup.empty() ? json(nullptr) : json(backup)}};
    return true;
}

} // namespace bmoe::server
