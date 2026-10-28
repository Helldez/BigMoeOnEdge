// The agent's tools as rules: where a path may resolve, which commands only read, what an edit
// looks like as a diff, and the tools that read the workspace (read_file, file_glob_search,
// grep_search) with the edit edit_file applies. Pure functions over strings and the filesystem, no
// session, no HTTP, so each rule is tested on its own (tests/agent_test.cpp). Names, parameters and
// result shapes follow llama.cpp's server tools, so a model used to one works with the other. What
// runs them, and when it may, is agent.h; docs/agent.md is the statement.
#pragma once

#include "json_util.h"

#include <cstddef>
#include <string>
#include <vector>

namespace bmoe::server::agent {

// Limits a tool works within. Each keeps one tool call from taking the context a whole conversation
// needs: a coding agent's model has a few tens of thousands of tokens for everything.
inline constexpr size_t k_tool_result_max_bytes = 12 * 1024; // a result as the model reads it (~3k tokens)
inline constexpr size_t k_shortened_result_bytes = 600;      // an old result, once the context ran short
inline constexpr int k_command_default_timeout_s = 10;       // exec_shell_command with no timeout given
inline constexpr int k_command_max_timeout_s = 120;          // the longest timeout a call may ask for
inline constexpr int k_glob_max_results = 100;               // entries file_glob_search returns
inline constexpr size_t k_walk_max_entries = 50000;          // entries one walk of a folder looks at
inline constexpr size_t k_grep_max_matches = 100;            // matching lines of one grep_search
inline constexpr size_t k_grep_max_line_bytes = 4096;        // the part of a line a pattern is tried on
inline constexpr size_t k_search_max_file_bytes = 2 << 20;   // files larger than this are not searched
inline constexpr size_t k_read_max_file_bytes = 8 << 20;     // a file read_file / edit_file opens
inline constexpr size_t k_preview_max_bytes = 16 << 20;      // a file the preview route serves

// Folders a walk lists but never descends into: what they hold is generated or vendored, and one
// of them would fill every result. Compared case-insensitively.
inline constexpr const char * k_walk_skip_dirs[] = {".git",   ".svn",  ".hg",    "node_modules", "__pycache__",
                                                    ".venv",  "venv",  "dist",   "build",        "target",
                                                    ".cache", ".idea", ".vscode"};

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

// `text` cut to at most `max` bytes on a character boundary, with a line saying how much was left
// out. Unchanged when it fits.
std::string cap_text(const std::string & text, size_t max);

// A NUL in the first bytes: a file a text tool must not read.
bool looks_binary(const std::string & bytes);

// A glob over a '/'-separated path: `*` and `?` stay inside one name, `**` crosses folders (`**/`
// also matches no folder at all), `[abc]`, `[a-z]` and `[!a]` match one character.
bool glob_match(const std::string & pattern, const std::string & path);
// As file_glob_search and grep_search apply include/exclude: a pattern with no '/' matches the
// entry's name at any depth; one with a '/' matches the path relative to the searched folder, from
// any depth unless it starts with "**/" or '/' (anchored there).
bool path_glob_match(const std::string & pattern, const std::string & rel_path);

// `edits` ([{old_text, new_text}], llama.cpp's edit_file) applied to `before`: each old_text found
// exactly once in the original, none overlapping another, all replaced at once. False with the
// reason, naming the edit by its index, when one is missing, repeated, overlapping or changes nothing.
bool apply_edits(const std::string & before, const json & edits, std::string & after, std::string & err);

// What a tool did: the text the model reads, and for a file, the diff and the path a UI previews.
struct ToolOutput {
    bool ok = true;
    std::string text;
    std::string diff;
    std::string file;
};

// The workspace tools that only read, on the arguments the model gave (llama.cpp's schemas).
ToolOutput read_file(const std::string & ws, const json & args);
ToolOutput file_glob_search(const std::string & ws, const json & args);
ToolOutput grep_search(const std::string & ws, const json & args);

// A whole file, refused past k_read_max_file_bytes.
bool read_whole(const std::string & path, std::string & out, std::string & err);
// `content` as the whole file, its folders created.
bool write_whole(const std::string & path, const std::string & content, std::string & err);

} // namespace bmoe::server::agent
