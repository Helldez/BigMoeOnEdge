#include "model_files.h"

#include <cctype>
#include <cstdio>
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

} // namespace bmoe::server
