#include "agent_tools.h"

#include "json_util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace bmoe::server::agent {

bool resolve_in_workspace(const std::string & root,
                          const std::string & requested,
                          std::string & out,
                          std::string & err) {
    std::error_code ec;
    const fs::path real_root = fs::canonical(fs::u8path(root), ec);
    if (ec) {
        err = "the workspace folder " + root + " no longer exists";
        return false;
    }
    fs::path p = fs::u8path(requested.empty() ? std::string(".") : requested);
    if (p.is_relative()) p = real_root / p;
    // weakly_canonical resolves every link and `..` along the part of the path that exists, so what
    // is compared is where the file really is, not how the request spelled it.
    const fs::path real = fs::weakly_canonical(p, ec);
    if (ec) {
        err = "cannot resolve " + requested;
        return false;
    }
    // A link that survives resolution points at nothing: following it to write would create a file
    // wherever it points.
    fs::path prefix;
    for (const fs::path & part : real) {
        prefix /= part;
        if (fs::is_symlink(fs::symlink_status(prefix, ec))) {
            err = requested + " goes through a symbolic link to something that does not exist";
            return false;
        }
    }
    const fs::path rel = real.lexically_relative(real_root);
    if (rel.empty() || *rel.begin() == "..") {
        err = requested + " is outside the workspace";
        return false;
    }
    out = real.u8string();
    return true;
}

std::string workspace_relative(const std::string & root, const std::string & abs) {
    std::error_code ec;
    const fs::path real_root = fs::weakly_canonical(fs::u8path(root), ec);
    fs::path rel = fs::u8path(abs).lexically_relative(real_root);
    if (rel.empty()) return abs;
    std::string s = rel.generic_u8string();
    return s;
}

// ---- the read-only command classifier ----

namespace {

// Programs that read and print, and nothing else, once the options below are refused. Kept small:
// a program missing here asks, which is the safe answer. git is not here: the repository's own
// configuration (a folder the agent may have been handed) can make even `git status` run a program
// (core.fsmonitor, a clean filter, diff.external), and no option on the line turns all of that off.
const std::set<std::string> & read_only_programs() {
    static const std::set<std::string> p = {
        "ls",     "cat",      "head",    "tail",     "wc",   "grep",  "egrep", "fgrep", "rg",     "find", "pwd",
        "file",   "stat",     "du",      "df",       "tree", "which", "echo",  "diff",  "cmp",    "date", "uname",
        "whoami", "basename", "dirname", "realpath", "sort", "nl",    "dir",   "type",  "findstr"};
    return p;
}

bool starts(const std::string & s, const char * p) {
    return s.compare(0, std::strlen(p), p) == 0;
}

// `a` is the long option `--<name>`, or an abbreviation of it at least `shortest` letters long (GNU
// programs take any unambiguous one: `sort --o=x` is `--output=x`).
bool long_option(const std::string & a, const std::string & name, size_t shortest) {
    if (!starts(a, "--")) return false;
    const std::string given = a.substr(2, a.find('=') == std::string::npos ? std::string::npos : a.find('=') - 2);
    return given.size() >= shortest && given.size() <= name.size() && name.compare(0, given.size(), given) == 0;
}

// Shell words, quotes honoured. False for an unterminated quote.
bool split_words(const std::string & s, std::vector<std::string> & out) {
    std::string cur;
    bool in_word = false;
    char quote = 0;
    for (char c : s) {
        if (quote) {
            if (c == quote)
                quote = 0;
            else
                cur += c;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            in_word = true;
        } else if (c == ' ' || c == '\t') {
            if (in_word) out.push_back(cur);
            cur.clear();
            in_word = false;
        } else {
            cur += c;
            in_word = true;
        }
    }
    if (quote) return false;
    if (in_word) out.push_back(cur);
    return true;
}

bool stage_reads_only(const std::vector<std::string> & w) {
    if (w.empty()) return false;
    const std::string & prog = w[0];
    // A path is a program this list cannot vouch for; an assignment changes what a program does.
    if (prog.find('/') != std::string::npos || prog.find('\\') != std::string::npos ||
        prog.find('=') != std::string::npos || !read_only_programs().count(prog))
        return false;
    for (size_t i = 1; i < w.size(); ++i) {
        const std::string & a = w[i];
        // The command runs in the workspace, but its arguments can name any file: one that may lead
        // out of it (absolute, home, parent, drive) asks, so a read that skips the question stays
        // inside the folder the file tools are confined to.
        if (starts(a, "/") || starts(a, "~") || a.find("..") != std::string::npos || (a.size() >= 2 && a[1] == ':') ||
            a.find("=/") != std::string::npos || a.find("=~") != std::string::npos)
            return false;
        if (prog == "find" && (a == "-exec" || a == "-execdir" || a == "-ok" || a == "-okdir" || a == "-delete" ||
                               starts(a, "-fprint") || a == "-fls"))
            return false;
        if (prog == "sort" && (long_option(a, "output", 1) || long_option(a, "compress-program", 2) ||
                               (a.size() > 1 && a[0] == '-' && a[1] != '-' && a.find('o') != std::string::npos)))
            return false;
        if (prog == "rg" && starts(a, "--pre")) return false;
        if (prog == "tree" && (a == "-o" || starts(a, "--output"))) return false;
        if (prog == "date" && (a == "-s" || long_option(a, "set", 1))) return false;
    }
    return true;
}

} // namespace

bool is_read_only_command(const std::string & command) {
    // Redirection, substitution, separators, background jobs, grouping: any of them and the line is
    // more than a pipeline of the programs above. Refused even inside quotes, where telling a
    // harmless one from an expanded one is the shell's business, not this list's.
    for (char c : command)
        if (std::strchr(";&><`$(){}\n\r", c)) return false;
    size_t start = 0;
    bool any = false;
    for (;;) {
        const size_t bar = command.find('|', start);
        const std::string stage = command.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        std::vector<std::string> words;
        if (!split_words(stage, words) || !stage_reads_only(words)) return false;
        any = true;
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return any;
}

// ---- unified diff ----

namespace {

struct Line {
    std::string text; // without its line break
    bool newline;     // whether it ended with one
    bool operator==(const Line & o) const { return text == o.text && newline == o.newline; }
};

std::vector<Line> split_lines(const std::string & s) {
    std::vector<Line> out;
    size_t i = 0;
    while (i < s.size()) {
        const size_t nl = s.find('\n', i);
        if (nl == std::string::npos) {
            out.push_back({s.substr(i), false});
            break;
        }
        out.push_back({s.substr(i, nl - i), true});
        i = nl + 1;
    }
    return out;
}

void emit(std::string & out, char mark, const Line & l) {
    out += mark;
    out += l.text;
    out += '\n';
    if (!l.newline) out += "\\ No newline at end of file\n";
}

} // namespace

std::string unified_diff(const std::string & path, const std::string & before, const std::string & after) {
    if (before == after) return "";
    constexpr size_t ctx = 3;
    const std::vector<Line> a = split_lines(before), b = split_lines(after);
    size_t p = 0;
    while (p < a.size() && p < b.size() && a[p] == b[p])
        ++p;
    size_t s = 0;
    while (s < a.size() - p && s < b.size() - p && a[a.size() - 1 - s] == b[b.size() - 1 - s])
        ++s;
    const size_t os = p > ctx ? p - ctx : 0;
    const size_t oe = std::min(a.size(), a.size() - s + ctx);
    const size_t ne = std::min(b.size(), b.size() - s + ctx);
    const size_t old_n = oe - os, new_n = ne - os;
    std::string out = (before.empty() ? std::string("--- /dev/null") : "--- a/" + path) + "\n+++ b/" + path + "\n";
    out += "@@ -" + std::to_string(old_n ? os + 1 : os) + "," + std::to_string(old_n) + " +" +
           std::to_string(new_n ? os + 1 : os) + "," + std::to_string(new_n) + " @@\n";
    for (size_t i = os; i < p; ++i)
        emit(out, ' ', a[i]);
    for (size_t i = p; i < a.size() - s; ++i)
        emit(out, '-', a[i]);
    for (size_t i = p; i < b.size() - s; ++i)
        emit(out, '+', b[i]);
    for (size_t i = a.size() - s; i < oe; ++i)
        emit(out, ' ', a[i]);
    return out;
}

std::string cap_text(const std::string & text, size_t max) {
    if (text.size() <= max) return text;
    const size_t keep = complete_utf8_prefix_size(text.substr(0, max));
    return text.substr(0, keep) + "\n[... " + std::to_string(text.size() - keep) + " more bytes not shown]";
}

bool looks_binary(const std::string & bytes) {
    const size_t n = std::min<size_t>(bytes.size(), 8192);
    return std::memchr(bytes.data(), '\0', n) != nullptr;
}

// ---- globs ----

namespace {

// The class `[...]` that starts at p[0] == '[' against `c`; `end` is set past its ']'. False with
// end == nullptr when the class is not closed (the '[' is then a plain character).
bool class_match(const char * p, char c, const char *& end) {
    const char * q = p + 1;
    const bool negate = *q == '!' || *q == '^';
    if (negate) ++q;
    const char * first = q;
    bool hit = false;
    while (*q && (*q != ']' || q == first)) {
        if (q[1] == '-' && q[2] && q[2] != ']') {
            if ((unsigned char) c >= (unsigned char) q[0] && (unsigned char) c <= (unsigned char) q[2]) hit = true;
            q += 3;
        } else {
            if (*q == c) hit = true;
            ++q;
        }
    }
    if (*q != ']') {
        end = nullptr;
        return false;
    }
    end = q + 1;
    return hit != negate;
}

bool glob_at(const char * p, const char * s) {
    for (;;) {
        if (!*p) return !*s;
        if (p[0] == '*' && p[1] == '*') {
            const char * rest = p + 2;
            if (*rest == '/' && glob_at(rest + 1, s)) return true; // `**/` as no folder at all
            for (const char * t = s;; ++t) {
                if (glob_at(rest, t)) return true;
                if (!*t) return false;
            }
        }
        if (*p == '*') {
            for (const char * t = s;; ++t) {
                if (glob_at(p + 1, t)) return true;
                if (!*t || *t == '/') return false;
            }
        }
        if (*p == '?') {
            if (!*s || *s == '/') return false;
            ++p;
            ++s;
            continue;
        }
        if (*p == '[') {
            const char * end = nullptr;
            const bool hit = *s && *s != '/' && class_match(p, *s, end);
            if (end) {
                if (!hit) return false;
                p = end;
                ++s;
                continue;
            }
        }
        if (*p != *s) return false;
        ++p;
        ++s;
    }
}

std::string lower(std::string s) {
    for (char & c : s)
        c = (char) std::tolower((unsigned char) c);
    return s;
}

std::string arg_string(const json & a, const char * key, const std::string & fallback = "") {
    return a.contains(key) && a[key].is_string() ? a[key].get<std::string>() : fallback;
}

long arg_int(const json & a, const char * key, long fallback) {
    return a.contains(key) && a[key].is_number() ? a[key].get<long>() : fallback;
}

bool arg_bool(const json & a, const char * key) {
    return a.contains(key) && a[key].is_boolean() && a[key].get<bool>();
}

ToolOutput fail(const std::string & why) {
    return {false, "Error: " + why, "", ""};
}

struct Entry {
    std::string rel; // relative to the walked folder, '/'-separated
    std::string abs;
    bool dir = false;
};

// Everything under `base`, `max_depth` levels down (0: no limit), junk folders listed but not
// entered, in path order. `truncated` when k_walk_max_entries stopped it.
std::vector<Entry> walk(const std::string & base, long max_depth, bool & truncated) {
    std::vector<Entry> out;
    truncated = false;
    std::error_code ec;
    const fs::path root = fs::u8path(base);
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (out.size() >= k_walk_max_entries) {
            truncated = true;
            break;
        }
        Entry e;
        e.abs = it->path().u8string();
        e.rel = it->path().lexically_relative(root).generic_u8string();
        // A link to a folder is not entered (the iterator does not follow it): it may lead out.
        e.dir = it->is_directory(ec) && !it->is_symlink(ec);
        if (e.dir) {
            const std::string name = lower(it->path().filename().u8string());
            bool skip = false;
            for (const char * s : k_walk_skip_dirs)
                skip = skip || name == s;
            if (skip || (max_depth > 0 && it.depth() + 1 >= max_depth)) it.disable_recursion_pending();
        }
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const Entry & a, const Entry & b) { return a.rel < b.rel; });
    return out;
}

} // namespace

bool glob_match(const std::string & pattern, const std::string & path) {
    return glob_at(pattern.c_str(), path.c_str());
}

bool path_glob_match(const std::string & pattern, const std::string & rel_path) {
    if (pattern.find('/') == std::string::npos) {
        const size_t slash = rel_path.rfind('/');
        return glob_match(pattern, slash == std::string::npos ? rel_path : rel_path.substr(slash + 1));
    }
    if (pattern[0] == '/') return glob_match(pattern.substr(1), rel_path);
    if (pattern.compare(0, 3, "**/") == 0 || pattern == "**") return glob_match(pattern, rel_path);
    return glob_match("**/" + pattern, rel_path);
}

// ---- edits ----

bool apply_edits(const std::string & before, const json & edits, std::string & after, std::string & err) {
    if (!edits.is_array() || edits.empty()) {
        err = "\"edits\" must be a non-empty array of {old_text, new_text}";
        return false;
    }
    struct Match {
        size_t index, at, len;
        std::string text;
    };
    std::vector<Match> matches;
    for (size_t i = 0; i < edits.size(); ++i) {
        const json & e = edits[i];
        const std::string old_text = e.is_object() ? arg_string(e, "old_text") : "";
        const std::string tag = "edits[" + std::to_string(i) + "]";
        if (!e.is_object() || !e.contains("new_text") || !e["new_text"].is_string()) {
            err = tag + " must be an object with old_text and new_text";
            return false;
        }
        if (old_text.empty()) {
            err = tag + ".old_text must not be empty";
            return false;
        }
        const size_t at = before.find(old_text);
        if (at == std::string::npos) {
            err = "could not find " + tag +
                  ".old_text, it must match the file's current content exactly: read the "
                  "file again and copy it";
            return false;
        }
        size_t count = 0;
        for (size_t p = at; p != std::string::npos; p = before.find(old_text, p + 1))
            ++count;
        if (count > 1) {
            err = "found " + std::to_string(count) + " occurrences of " + tag +
                  ".old_text, it must be unique: include more surrounding lines";
            return false;
        }
        matches.push_back({i, at, old_text.size(), e["new_text"].get<std::string>()});
    }
    std::sort(matches.begin(), matches.end(), [](const Match & a, const Match & b) { return a.at < b.at; });
    for (size_t i = 1; i < matches.size(); ++i)
        if (matches[i - 1].at + matches[i - 1].len > matches[i].at) {
            err = "edits[" + std::to_string(matches[i - 1].index) + "] and edits[" + std::to_string(matches[i].index) +
                  "] overlap; merge them into one edit or target disjoint regions";
            return false;
        }
    after.clear();
    size_t from = 0;
    for (const Match & m : matches) {
        after.append(before, from, m.at - from);
        after += m.text;
        from = m.at + m.len;
    }
    after.append(before, from, std::string::npos);
    if (after == before) {
        err = "no changes made: the replacement(s) produced identical content";
        return false;
    }
    return true;
}

// ---- files ----

bool read_whole(const std::string & path, std::string & out, std::string & err) {
    std::error_code ec;
    const auto size = fs::file_size(fs::u8path(path), ec);
    if (ec) {
        err = "cannot read the file (" + ec.message() + ")";
        return false;
    }
    if (size > k_read_max_file_bytes) {
        err = "the file is " + std::to_string(size) + " bytes, more than the " + std::to_string(k_read_max_file_bytes) +
              " a tool reads";
        return false;
    }
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (!f && !f.eof()) {
        err = "cannot read the file";
        return false;
    }
    return true;
}

bool write_whole(const std::string & path, const std::string & content, std::string & err) {
    std::error_code ec;
    fs::create_directories(fs::u8path(path).parent_path(), ec);
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
    f << content;
    if (!f) {
        err = "cannot write the file";
        return false;
    }
    return true;
}

ToolOutput read_file(const std::string & ws, const json & a) {
    std::string path, err, text;
    if (!resolve_in_workspace(ws, arg_string(a, "path"), path, err)) return fail(err);
    std::error_code ec;
    if (fs::is_directory(fs::u8path(path), ec))
        return fail(arg_string(a, "path") + " is a folder: use file_glob_search");
    if (!read_whole(path, text, err)) return fail(err);
    if (looks_binary(text)) return fail(arg_string(a, "path") + " is not a text file");
    const long first = std::max(1L, arg_int(a, "start_line", 1));
    const long last = arg_int(a, "end_line", -1);
    const bool loc = arg_bool(a, "append_loc");
    std::string out;
    long n = 0, cut_at = 0;
    size_t i = 0;
    while (i < text.size()) {
        const size_t nl = text.find('\n', i);
        const std::string line = text.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        i = nl == std::string::npos ? text.size() : nl + 1;
        ++n;
        if (n < first || (last > 0 && n > last) || cut_at) continue;
        // "N→text", the numbering llama.cpp's read_file uses.
        const std::string piece = (loc ? std::to_string(n) + "\xE2\x86\x92" : std::string()) + line + "\n";
        if (out.size() + piece.size() > k_tool_result_max_bytes) {
            cut_at = n;
            continue;
        }
        out += piece;
    }
    if (cut_at)
        out += "[output truncated at line " + std::to_string(cut_at) + " of " + std::to_string(n) +
               ": read on with start_line=" + std::to_string(cut_at) + "]";
    else if (out.empty())
        out = n == 0 ? "(empty file)" : "(no lines in that range: the file has " + std::to_string(n) + ")";
    return {true, out, "", workspace_relative(ws, path)};
}

ToolOutput file_glob_search(const std::string & ws, const json & a) {
    const std::string req = arg_string(a, "path", ".");
    std::string base, err;
    if (!resolve_in_workspace(ws, req, base, err)) return fail(err);
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(base), ec)) return fail(req + " is not a folder");
    const std::string include = arg_string(a, "include", "**");
    const std::string exclude = arg_string(a, "exclude");
    const std::string type = arg_string(a, "type", "file");
    if (type != "file" && type != "dir" && type != "all")
        return fail("invalid type: " + type + " (expected \"file\", \"dir\" or \"all\")");
    const long limit_req = arg_int(a, "limit", k_glob_max_results);
    if (limit_req < 1) return fail("invalid limit: " + std::to_string(limit_req) + " (expected 1 or more)");
    const size_t limit = (size_t) std::min<long>(limit_req, k_glob_max_results);
    bool truncated = false;
    std::string out;
    size_t total = 0;
    for (const Entry & e : walk(base, std::max(0L, arg_int(a, "max_depth", 0)), truncated)) {
        if ((e.dir && type == "file") || (!e.dir && type == "dir")) continue;
        if (!path_glob_match(include, e.rel) || (!exclude.empty() && path_glob_match(exclude, e.rel))) continue;
        if (total++ < limit) out += e.rel + (e.dir ? "/" : "") + "\n";
    }
    out += "\n---\nTotal matches: " + std::to_string(total) + "\n";
    if (total > limit)
        out += "[" + std::to_string(limit) + " results limit reached (" + std::to_string(total) +
               " total matches). Refine the glob pattern to narrow the search.]\n";
    if (truncated)
        out += "[results truncated: the folder holds more than " + std::to_string(k_walk_max_entries) + " entries]\n";
    return {true, out, "", ""};
}

ToolOutput grep_search(const std::string & ws, const json & a) {
    const std::string req = arg_string(a, "path", ".");
    const std::string pattern = arg_string(a, "pattern");
    if (pattern.empty()) return fail("pattern is empty");
    std::string src = pattern;
    if (arg_bool(a, "literal")) {
        src.clear();
        for (char c : pattern) {
            if (std::strchr("\\^$.|?*+()[]{}", c)) src += '\\';
            src += c;
        }
    }
    std::regex re;
    try {
        auto flags = std::regex::ECMAScript;
        if (arg_bool(a, "ignore_case")) flags |= std::regex::icase;
        re = std::regex(src, flags);
    } catch (const std::regex_error & e) {
        return fail(std::string("invalid regex: ") + e.what());
    }
    std::string base, err;
    if (!resolve_in_workspace(ws, req, base, err)) return fail(err);
    const std::string include = arg_string(a, "include", "**");
    const std::string exclude = arg_string(a, "exclude");
    const long ctx = std::max(0L, arg_int(a, "context_lines", 0));
    const bool numbers = arg_bool(a, "return_line_numbers") || ctx > 0;
    std::vector<std::pair<std::string, std::string>> files; // absolute, as shown
    std::error_code ec;
    if (fs::is_regular_file(fs::u8path(base), ec)) {
        files.emplace_back(base, req);
    } else if (fs::is_directory(fs::u8path(base), ec)) {
        bool truncated = false;
        for (const Entry & e : walk(base, 0, truncated)) {
            if (e.dir || !path_glob_match(include, e.rel) || (!exclude.empty() && path_glob_match(exclude, e.rel)))
                continue;
            // A link may lead out of the workspace; only what resolves inside it is read.
            std::string real;
            if (!resolve_in_workspace(ws, e.abs, real, err)) continue;
            files.emplace_back(real, e.rel);
        }
    } else {
        return fail("path does not exist: " + req);
    }
    std::string out;
    size_t total = 0;
    bool full = false;
    for (const auto & [abs, shown] : files) {
        if (full) break;
        if (fs::file_size(fs::u8path(abs), ec) > k_search_max_file_bytes || ec) continue;
        std::string text;
        if (!read_whole(abs, text, err) || looks_binary(text)) continue;
        std::vector<std::string> lines;
        std::istringstream in(text);
        for (std::string l; std::getline(in, l);)
            lines.push_back(l);
        for (size_t i = 0; i < lines.size(); ++i) {
            // A pattern is tried on the start of a very long line only: std::regex recurses per
            // character, and a minified file's one line would exhaust the stack.
            const std::string & l = lines[i];
            const bool hit = l.size() <= k_grep_max_line_bytes
                                 ? std::regex_search(l, re)
                                 : std::regex_search(l.begin(), l.begin() + k_grep_max_line_bytes, re);
            if (!hit) continue;
            if (total >= k_grep_max_matches) {
                full = true;
                break;
            }
            ++total;
            const size_t from = i >= (size_t) ctx ? i - ctx : 0, to = std::min(lines.size() - 1, i + ctx);
            for (size_t j = from; j <= to; ++j) {
                const char mark = j == i ? ':' : '-';
                out += shown + mark + (numbers ? std::to_string(j + 1) + mark : std::string()) +
                       cap_text(lines[j], 500) + "\n";
            }
            if (ctx > 0) out += "--\n";
        }
    }
    out += "\n---\nTotal matches: " + std::to_string(total) + "\n";
    if (full)
        out += "[" + std::to_string(k_grep_max_matches) +
               " matches limit reached. Narrow the path/pattern/include to see more.]\n";
    return {true, out, "", ""};
}

} // namespace bmoe::server::agent
