#include "model_files.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>

namespace bmoe::server {

namespace {

bool ends_with_nocase(const std::string & s, const char * suffix) {
    const std::string suf(suffix);
    if (s.size() < suf.size()) return false;
    for (size_t i = 0; i < suf.size(); ++i)
        if (std::tolower((unsigned char) s[s.size() - suf.size() + i]) != suf[i]) return false;
    return true;
}

// Five decimal digits at `at`, as an int; -1 when they are not.
int five_digits(const std::string & s, size_t at) {
    if (at + 5 > s.size()) return -1;
    int v = 0;
    for (size_t i = at; i < at + 5; ++i) {
        if (!std::isdigit((unsigned char) s[i])) return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

} // namespace

bool is_gguf_name(const std::string & name) {
    return ends_with_nocase(name, ".gguf");
}

bool parse_shard_name(const std::string & name, ShardName & out) {
    // "-NNNNN-of-NNNNN.gguf" is 20 characters, after a stem of at least one.
    static constexpr size_t tail = 20;
    if (name.size() <= tail || !is_gguf_name(name)) return false;
    const size_t b = name.size() - tail;
    const auto at = [&](size_t i, char c) { return std::tolower((unsigned char) name[b + i]) == c; };
    if (!at(0, '-') || !at(6, '-') || !at(7, 'o') || !at(8, 'f') || !at(9, '-')) return false;
    const int index = five_digits(name, b + 1);
    const int count = five_digits(name, b + 10);
    if (index < 1 || count < 1 || index > count) return false;
    out.stem = name.substr(0, b);
    out.index = index;
    out.count = count;
    return true;
}

std::string shard_file_name(const std::string & stem, int index, int count) {
    char tail[32];
    std::snprintf(tail, sizeof(tail), "-%05d-of-%05d.gguf", index, count);
    return stem + tail;
}

std::vector<ModelFiles> group_model_files(const std::vector<std::string> & names) {
    struct Set {
        int count = 1;
        std::map<int, std::string> present;
    };
    // Keyed by the first file's name: a set and its shards meet under one key, and the listing
    // comes out sorted by what a person reads.
    std::map<std::string, Set> sets;
    for (const std::string & name : names) {
        if (!is_gguf_name(name)) continue;
        ShardName sh;
        if (parse_shard_name(name, sh)) {
            Set & s = sets[shard_file_name(sh.stem, 1, sh.count)];
            s.count = sh.count;
            s.present[sh.index] = name;
        } else {
            sets[name].present[1] = name;
        }
    }
    std::vector<ModelFiles> out;
    for (auto & kv : sets) {
        ModelFiles m;
        // The name on disk where shard 1 is there (its case may differ from the canonical one).
        const auto one = kv.second.present.find(1);
        m.first = one != kv.second.present.end() ? one->second : kv.first;
        m.count = kv.second.count;
        for (auto & f : kv.second.present)
            m.files.push_back(f.second);
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<ModelFiles> scan_model_files(const std::string & dir) {
    namespace fs = std::filesystem;
    std::vector<ModelFiles> out;
    const auto add_folder = [&out](const fs::path & where, const std::string & folder) {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto & ent : fs::directory_iterator(where, ec))
            if (ent.is_regular_file(ec)) names.push_back(ent.path().filename().u8string());
        for (ModelFiles & m : group_model_files(names)) {
            m.folder = folder;
            out.push_back(std::move(m));
        }
    };
    const fs::path base = fs::u8path(dir);
    add_folder(base, "");
    std::vector<std::string> subs;
    std::error_code ec;
    for (const auto & ent : fs::directory_iterator(base, ec)) {
        // is_symlink first: is_directory follows a link, and a link is how a scan loops or leaves.
        std::error_code e;
        if (ent.is_symlink(e) || !ent.is_directory(e)) continue;
        subs.push_back(ent.path().filename().u8string());
    }
    std::sort(subs.begin(), subs.end());
    for (const std::string & sub : subs)
        add_folder(base / fs::u8path(sub), sub);
    return out;
}

const char * file_role_name(FileRole r) {
    switch (r) {
    case FileRole::Model:
        return "model";
    case FileRole::Projector:
        return "projector";
    case FileRole::Draft:
        return "draft";
    case FileRole::Other:
        return "other";
    case FileRole::Unknown:
        break;
    }
    return "";
}

namespace {

// llama.cpp's multimodal projector files: `general.type` "mmproj" in every recent conversion, and
// the "clip" architecture, which is the projector format itself (older files carry no type).
constexpr const char * k_projector_type = "mmproj";
constexpr const char * k_projector_arch = "clip";
constexpr const char * k_model_type = "model";

// Name conventions for companion files, for ordering only (file_role_hint).
constexpr const char * k_projector_name_mark = "mmproj";
constexpr const char * k_draft_name_prefixes[] = {"mtp-", "dflash-", "draft-"};

std::string lower(std::string s) {
    for (char & c : s)
        c = (char) std::tolower((unsigned char) c);
    return s;
}

} // namespace

FileRole file_role(const GgufHeaderFacts & h) {
    if (!h.ok) return FileRole::Unknown;
    if (h.type == k_projector_type || h.arch == k_projector_arch) return FileRole::Projector;
    if (!h.type.empty() && h.type != k_model_type) return FileRole::Other;
    if (h.n_block <= 0) return FileRole::Other;
    if (h.reads_target || h.n_nextn >= h.n_block) return FileRole::Draft;
    return FileRole::Model;
}

FileRole file_role_hint(const std::string & name) {
    const size_t slash = name.find_last_of('/');
    const std::string base = lower(slash == std::string::npos ? name : name.substr(slash + 1));
    if (base.find(k_projector_name_mark) != std::string::npos) return FileRole::Projector;
    for (const char * p : k_draft_name_prefixes)
        if (base.compare(0, std::char_traits<char>::length(p), p) == 0) return FileRole::Draft;
    return FileRole::Model;
}

} // namespace bmoe::server
