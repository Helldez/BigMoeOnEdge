// Skills: a folder with a SKILL.md, front matter naming it and saying when it helps, then the
// instructions. Parsed defensively, since a person writes them by hand and a workspace brings its
// own: a file that does not parse is skipped with a warning, never a failed listing.
#pragma once

#include <string>
#include <vector>

namespace bmoe::server::agent {

// A name is kebab-case: lowercase letters, digits and single hyphens, at most this long. It is the
// folder's name too, so it can never name a path.
inline constexpr size_t k_skill_name_max = 64;
inline constexpr size_t k_skill_description_max = 1024;
inline constexpr size_t k_skill_body_max = 64 * 1024;

struct Skill {
    std::string name;
    std::string title;       // front matter `title`, or the name in words
    std::string description; // when the skill helps: what the model reads to decide
    std::string body;        // the instructions, read with use_skill
    std::string source;      // "bundled", "user" or "workspace"
    std::string dir;         // the folder it was read from
};

bool valid_skill_name(const std::string & name);

// A SKILL.md's text as a skill: `---` front matter with `name` and `description` (plain, quoted, or
// a `>` / `|` block of indented lines; other keys ignored), then the body. False with the reason.
bool parse_skill(const std::string & text, Skill & out, std::string & err);

// The SKILL.md text for `s`: what parse_skill reads back as the same name, description and body.
std::string render_skill(const Skill & s);

// Every `<dir>/<name>/SKILL.md`, as `source`, sorted by name. A file whose front matter names a
// different skill than its folder is skipped: the folder is how it is found again.
std::vector<Skill>
scan_skills(const std::string & dir, const std::string & source, std::vector<std::string> & warnings);

} // namespace bmoe::server::agent
