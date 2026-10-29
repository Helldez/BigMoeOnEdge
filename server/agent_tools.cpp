#include "agent_tools.h"

#include "json_util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>

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

// ---- web pages ----

namespace {

std::string lower(std::string s) {
    for (char & c : s)
        c = (char) std::tolower((unsigned char) c);
    return s;
}

void append_utf8(std::string & out, unsigned long cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;
    if (cp < 0x80) {
        out += (char) cp;
    } else if (cp < 0x800) {
        out += (char) (0xC0 | (cp >> 6));
        out += (char) (0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char) (0xE0 | (cp >> 12));
        out += (char) (0x80 | ((cp >> 6) & 0x3F));
        out += (char) (0x80 | (cp & 0x3F));
    } else {
        out += (char) (0xF0 | (cp >> 18));
        out += (char) (0x80 | ((cp >> 12) & 0x3F));
        out += (char) (0x80 | ((cp >> 6) & 0x3F));
        out += (char) (0x80 | (cp & 0x3F));
    }
}

std::string decode_entities(const std::string & s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += '&';
            continue;
        }
        const std::string name = s.substr(i + 1, semi - i - 1);
        std::string rep;
        bool known = true;
        if (name == "amp")
            rep = "&";
        else if (name == "lt")
            rep = "<";
        else if (name == "gt")
            rep = ">";
        else if (name == "quot")
            rep = "\"";
        else if (name == "apos")
            rep = "'";
        else if (name == "nbsp")
            rep = " ";
        else if (name.size() > 1 && name[0] == '#') {
            char * end = nullptr;
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const unsigned long cp = std::strtoul(name.c_str() + (hex ? 2 : 1), &end, hex ? 16 : 10);
            if (end && *end == '\0')
                append_utf8(rep, cp);
            else
                known = false;
        } else
            known = false;
        if (!known) {
            out += '&';
            continue;
        }
        out += rep;
        i = semi;
    }
    return out;
}

bool is_block_tag(const std::string & t) {
    static const std::set<std::string> blocks = {
        "p",  "div", "br",      "li",      "h1",     "h2",     "h3",  "h4",         "h5",
        "h6", "tr",  "section", "article", "header", "footer", "pre", "blockquote", "table",
        "ul", "ol",  "nav",     "main",    "aside",  "hr",     "dt",  "dd",         "title"};
    return blocks.count(t) > 0;
}

} // namespace

std::string html_to_text(const std::string & html) {
    const std::string low = lower(html);
    std::string raw;
    raw.reserve(html.size() / 2);
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] != '<') {
            raw += html[i++];
            continue;
        }
        if (low.compare(i, 4, "<!--") == 0) {
            const size_t e = low.find("-->", i + 4);
            i = e == std::string::npos ? html.size() : e + 3;
            continue;
        }
        const size_t close = html.find('>', i);
        if (close == std::string::npos) break;
        size_t n = i + 1;
        const bool end_tag = n < html.size() && html[n] == '/';
        if (end_tag) ++n;
        size_t ne = n;
        while (ne < close && (std::isalnum((unsigned char) html[ne]) || html[ne] == '-'))
            ++ne;
        const std::string tag = low.substr(n, ne - n);
        // What a browser never shows: skipped to its end tag whole.
        if (!end_tag && (tag == "script" || tag == "style" || tag == "noscript" || tag == "svg" || tag == "template" ||
                         tag == "head")) {
            const size_t e = low.find("</" + tag, close);
            if (e == std::string::npos) break;
            const size_t ee = html.find('>', e);
            i = ee == std::string::npos ? html.size() : ee + 1;
            raw += '\n';
            continue;
        }
        if (tag == "li")
            raw += end_tag ? "" : "\n- ";
        else if (is_block_tag(tag) || tag == "td" || tag == "th")
            raw += tag == "td" || tag == "th" ? " " : "\n";
        i = close + 1;
    }
    const std::string text = decode_entities(raw);
    // Blank space as a reader sees it: one space inside a line, no line of only spaces, and at most
    // one empty line between paragraphs.
    std::string out;
    std::string line;
    int blank = 0;
    const auto flush = [&] {
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        if (line.empty()) {
            if (!out.empty() && blank++ == 0) out += '\n';
        } else {
            blank = 0;
            out += line;
            out += '\n';
        }
        line.clear();
    };
    for (char c : text) {
        if (c == '\n') {
            flush();
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            if (!line.empty() && line.back() != ' ') line += ' ';
        } else {
            line += c;
        }
    }
    flush();
    while (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

namespace {

std::string attribute(const std::string & tag, const std::string & name) {
    const std::string low = lower(tag);
    size_t at = 0;
    while ((at = low.find(name + "=", at)) != std::string::npos) {
        if (at > 0 && !std::isspace((unsigned char) low[at - 1])) {
            ++at;
            continue;
        }
        size_t v = at + name.size() + 1;
        if (v >= tag.size()) return "";
        const char q = tag[v];
        if (q == '"' || q == '\'') {
            const size_t e = tag.find(q, v + 1);
            if (e == std::string::npos) return "";
            return tag.substr(v + 1, e - v - 1);
        }
        size_t e = v;
        while (e < tag.size() && !std::isspace((unsigned char) tag[e]) && tag[e] != '>')
            ++e;
        return tag.substr(v, e - v);
    }
    return "";
}

// The text of the element whose start tag ends at `open_end`, up to `</name`.
std::string inner_text(const std::string & html, const std::string & low, size_t open_end, const std::string & name) {
    const size_t e = low.find("</" + name, open_end);
    if (e == std::string::npos) return "";
    std::string t = html_to_text(html.substr(open_end + 1, e - open_end - 1));
    for (char & c : t)
        if (c == '\n') c = ' ';
    return t;
}

std::string unwrap_link(std::string href) {
    href = decode_entities(href);
    const size_t u = href.find("uddg=");
    if (u != std::string::npos) {
        const size_t amp = href.find('&', u);
        href = url_decode(href.substr(u + 5, amp == std::string::npos ? std::string::npos : amp - u - 5));
    }
    if (starts(href, "//")) href = "https:" + href;
    return href;
}

} // namespace

std::vector<SearchHit> parse_search_results(const std::string & html, size_t max_results) {
    std::vector<SearchHit> hits;
    const std::string low = lower(html);
    size_t at = 0;
    while (hits.size() < max_results) {
        const size_t mark = low.find("result__a", at);
        if (mark == std::string::npos) break;
        at = mark + 9;
        const size_t open = low.rfind("<a", mark);
        const size_t open_end = low.find('>', mark);
        if (open == std::string::npos || open_end == std::string::npos || open_end < open) continue;
        const std::string tag = html.substr(open, open_end - open + 1);
        // The class must belong to this anchor, not to text before it.
        if (low.find('>', open) != open_end) continue;
        SearchHit h;
        h.url = unwrap_link(attribute(tag, "href"));
        h.title = inner_text(html, low, open_end, "a");
        // An advertisement goes through the engine's own click tracker, not to a page.
        if ((!starts(h.url, "https://") && !starts(h.url, "http://")) ||
            h.url.find("duckduckgo.com/y.js") != std::string::npos || h.title.empty())
            continue;
        const size_t next = low.find("result__a", at);
        const size_t snip = low.find("result__snippet", at);
        if (snip != std::string::npos && (next == std::string::npos || snip < next)) {
            const size_t s_open = low.rfind('<', snip);
            const size_t s_end = low.find('>', snip);
            if (s_open != std::string::npos && s_end != std::string::npos) {
                size_t ne = s_open + 1;
                while (ne < low.size() && std::isalpha((unsigned char) low[ne]))
                    ++ne;
                h.snippet = inner_text(html, low, s_end, low.substr(s_open + 1, ne - s_open - 1));
            }
        }
        hits.push_back(std::move(h));
    }
    return hits;
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

std::string url_decode(const std::string & s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char) s[i + 1]) &&
                   std::isxdigit((unsigned char) s[i + 2])) {
            out += (char) std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string url_encode(const std::string & s) {
    static const char * hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char) c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool url_names_private_host(const std::string & url) {
    size_t at = url.find("://");
    std::string host = url.substr(at == std::string::npos ? 0 : at + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    const size_t cred = host.rfind('@');
    if (cred != std::string::npos) host = host.substr(cred + 1);
    if (!host.empty() && host[0] == '[') {
        // An IPv6 literal: loopback, unspecified, unique-local (fc00::/7), link-local (fe80::/10),
        // or an IPv4 address mapped into it.
        host = lower(host.substr(1, host.find(']') - 1));
        if (host == "::1" || host == "::" || starts(host, "fc") || starts(host, "fd") || starts(host, "fe8") ||
            starts(host, "fe9") || starts(host, "fea") || starts(host, "feb"))
            return true;
        if (starts(host, "::ffff:"))
            host = host.substr(7);
        else
            return false;
    } else {
        host = lower(host.substr(0, host.find(':')));
    }
    while (!host.empty() && host.back() == '.')
        host.pop_back();
    const auto ends = [&](const char * s) {
        const size_t n = std::strlen(s);
        return host.size() >= n && host.compare(host.size() - n, n, s) == 0;
    };
    if (host.empty() || host == "localhost" || ends(".localhost") || ends(".local") || ends(".internal") ||
        ends(".lan") || ends(".home.arpa"))
        return true;
    unsigned a[4];
    char tail = 0;
    if (std::sscanf(host.c_str(), "%u.%u.%u.%u%c", &a[0], &a[1], &a[2], &a[3], &tail) != 4) {
        // A bare number is an address to curl too (2130706433 and 0x7f000001 are 127.0.0.1).
        return host.find_first_not_of("0123456789") == std::string::npos ||
               (starts(host, "0x") && host.find_first_not_of("0123456789abcdef", 2) == std::string::npos);
    }
    // An octet written with a leading zero is octal to some resolvers: not a form a public page uses.
    for (size_t i = 0; i + 1 < host.size(); ++i)
        if (host[i] == '0' && (i == 0 || host[i - 1] == '.') && std::isdigit((unsigned char) host[i + 1])) return true;
    return a[0] == 0 || a[0] == 10 || a[0] == 127 || (a[0] == 169 && a[1] == 254) ||
           (a[0] == 172 && a[1] >= 16 && a[1] <= 31) || (a[0] == 192 && a[1] == 168) ||
           (a[0] == 100 && a[1] >= 64 && a[1] <= 127);
}

} // namespace bmoe::server::agent
