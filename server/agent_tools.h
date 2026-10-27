// The agent's tools as rules: where a path may resolve, which commands only read, what an edit
// looks like as a diff, a web page as text, a search page as results. Pure functions over strings
// and the filesystem, no session, no HTTP, so each rule is tested on its own (tests/agent_test.cpp).
// What runs them, and when it may, is agent.h; docs/agent.md is the statement.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace bmoe::server::agent {

// Limits a tool works within. Each keeps one tool call from taking the context a whole conversation
// needs: a coding agent's model has a few tens of thousands of tokens for everything.
inline constexpr size_t k_tool_result_max_bytes = 12 * 1024; // a result as the model reads it (~3k tokens)
inline constexpr size_t k_shortened_result_bytes = 600;      // an old result, once the context ran short
inline constexpr int k_command_timeout_s = 120;              // a command longer than this is ended
inline constexpr size_t k_list_max_entries = 400;            // a folder listing
inline constexpr size_t k_search_max_hits = 200;             // matching lines of one search
inline constexpr size_t k_search_max_file_bytes = 2 << 20;   // files larger than this are not searched
inline constexpr size_t k_read_max_file_bytes = 8 << 20;     // a file read_file / edit_file opens
inline constexpr size_t k_web_max_page_bytes = 2 << 20;      // bytes of a page read before it is cut off
inline constexpr int k_web_timeout_s = 30;                   // one web request
inline constexpr size_t k_search_max_results = 8;            // results web_search returns
inline constexpr size_t k_preview_max_bytes = 16 << 20;      // a file the preview route serves

// The keyless search page web_search reads: an HTML results page that needs no account and no key,
// parsed defensively (parse_search_results). One constant, so a change of endpoint is one line.
inline constexpr const char * k_web_search_endpoint = "https://html.duckduckgo.com/html/";

// Folders search_files and list_files' recursion never walk into: what they hold is generated, and
// one of them would fill every result.
inline constexpr const char * k_search_skip_dirs[] = {".git", "node_modules", ".venv", "__pycache__"};

// `requested` (relative to the workspace, or absolute) as the path a tool may touch: made canonical,
// every `..` and symbolic link along its existing part resolved, and inside `root`. A file that does
// not exist yet resolves through its nearest existing folder. Refused, with the reason: anything that
// resolves outside, and a dangling symbolic link (writing through it would create its target, which
// may be anywhere). `out` is absolute.
bool resolve_in_workspace(const std::string & root,
                          const std::string & requested,
                          std::string & out,
                          std::string & err);

// `abs` as the workspace-relative path a person reads ("src/main.cpp"), with forward slashes.
std::string workspace_relative(const std::string & root, const std::string & abs);

// Whether `command` can be told to only read: every stage of a pipeline is a program on the
// read-only list with no option that writes or runs something, and the line has no redirection, no
// substitution and no command separator. Anything it cannot tell is false, and asks.
bool is_read_only_command(const std::string & command);

// The change from `before` to `after` as a unified diff of `path` (`--- a/path`, `+++ b/path`, one
// hunk around the lines that differ, three lines of context). "" when they are equal.
std::string unified_diff(const std::string & path, const std::string & before, const std::string & after);

// A web page as the text a person reads: scripts, styles and markup gone, block elements on lines
// of their own, the common entities decoded, runs of blank space collapsed.
std::string html_to_text(const std::string & html);

struct SearchHit {
    std::string title;
    std::string url;
    std::string snippet;
};
// The results of k_web_search_endpoint's page. A page that does not look as expected gives no
// results rather than wrong ones; the search engine's own redirect links are unwrapped.
std::vector<SearchHit> parse_search_results(const std::string & html, size_t max_results = k_search_max_results);

// `text` cut to at most `max` bytes on a character boundary, with a line saying how much was left
// out. Unchanged when it fits.
std::string cap_text(const std::string & text, size_t max);

// A NUL in the first bytes: a file a text tool must not read.
bool looks_binary(const std::string & bytes);

// `s` with %XX and '+' decoded, as a query string writes it.
std::string url_decode(const std::string & s);
std::string url_encode(const std::string & s);

} // namespace bmoe::server::agent
