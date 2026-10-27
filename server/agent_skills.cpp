#include "agent_skills.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace bmoe::server::agent {

bool valid_skill_name(const std::string & name) {
    if (name.empty() || name.size() > k_skill_name_max || name.front() == '-' || name.back() == '-') return false;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '-' && name[i - 1] == '-') return false;
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-') return false;
    }
    return true;
}

namespace {

std::string trim(const std::string & s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char) s[b]))
        ++b;
    while (e > b && std::isspace((unsigned char) s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

std::string unquote(const std::string & v) {
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) {
        std::string out;
        for (size_t i = 1; i + 1 < v.size(); ++i) {
            if (v.front() == '"' && v[i] == '\\' && i + 2 < v.size()) {
                const char n = v[++i];
                out += n == 'n' ? '\n' : n;
            } else {
                out += v[i];
            }
        }
        return out;
    }
    return v;
}

std::string words_of(const std::string & name) {
    std::string t = name;
    for (char & c : t)
        if (c == '-') c = ' ';
    if (!t.empty()) t[0] = (char) std::toupper((unsigned char) t[0]);
    return t;
}

} // namespace

bool parse_skill(const std::string & text_in, Skill & out, std::string & err) {
    std::string text = text_in;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3); // a byte-order mark an editor added
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || trim(line) != "---") {
        err = "SKILL.md must start with a --- front matter block";
        return false;
    }
    std::vector<std::string> fm;
    bool closed = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line) == "---") {
            closed = true;
            break;
        }
        fm.push_back(line);
    }
    if (!closed) {
        err = "the front matter is not closed with ---";
        return false;
    }
    std::string name, description, title;
    for (size_t i = 0; i < fm.size(); ++i) {
        const std::string & l = fm[i];
        if (l.empty() || std::isspace((unsigned char) l[0]) || l[0] == '#') continue;
        const size_t colon = l.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(l.substr(0, colon));
        std::string value = trim(l.substr(colon + 1));
        if (value == ">" || value == "|" || value == ">-" || value == "|-") {
            // A block: the indented lines that follow, folded into one paragraph or kept as lines.
            const bool fold = value[0] == '>';
            std::string block;
            while (i + 1 < fm.size() && (fm[i + 1].empty() || std::isspace((unsigned char) fm[i + 1][0]))) {
                const std::string part = trim(fm[++i]);
                if (!block.empty()) block += fold ? " " : "\n";
                block += part;
            }
            value = trim(block);
        } else {
            value = unquote(value);
        }
        if (key == "name")
            name = value;
        else if (key == "description")
            description = value;
        else if (key == "title")
            title = value;
    }
    if (!valid_skill_name(name)) {
        err = "the name must be kebab-case (lowercase letters, digits, hyphens), at most " +
              std::to_string(k_skill_name_max) + " characters";
        return false;
    }
    if (description.empty()) {
        err = "the description is missing: it is how the agent knows when the skill helps";
        return false;
    }
    if (description.size() > k_skill_description_max) description.resize(k_skill_description_max);
    std::ostringstream rest;
    rest << in.rdbuf();
    std::string body = rest.str();
    if (body.size() > k_skill_body_max) {
        err = "the instructions are longer than " + std::to_string(k_skill_body_max) + " bytes";
        return false;
    }
    out.name = name;
    out.description = description;
    out.title = title.empty() ? words_of(name) : title;
    out.body = trim(body);
    return true;
}

std::string render_skill(const Skill & s) {
    // Quoted, so a colon or a leading symbol in the description reads back as the same text.
    std::string d;
    for (char c : s.description) {
        if (c == '"' || c == '\\') d += '\\';
        if (c == '\n') {
            d += "\\n";
            continue;
        }
        d += c;
    }
    std::string out = "---\nname: " + s.name + "\n";
    if (!s.title.empty() && s.title != words_of(s.name)) {
        std::string t;
        for (char c : s.title)
            if (c != '\n') t += c == '"' ? '\'' : c;
        out += "title: \"" + t + "\"\n";
    }
    out += "description: \"" + d + "\"\n---\n\n" + s.body;
    if (out.back() != '\n') out += '\n';
    return out;
}

std::vector<Skill>
scan_skills(const std::string & dir, const std::string & source, std::vector<std::string> & warnings) {
    std::vector<Skill> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(fs::u8path(dir), ec)) return out;
    for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const fs::path md = it->path() / "SKILL.md";
        if (!fs::is_regular_file(md, ec)) continue;
        if (fs::file_size(md, ec) > k_skill_body_max + k_skill_description_max + 4096) {
            warnings.push_back(md.u8string() + ": too large to be a skill");
            continue;
        }
        std::ifstream f(md, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        Skill s;
        std::string err;
        if (!parse_skill(ss.str(), s, err)) {
            warnings.push_back(md.u8string() + ": " + err);
            continue;
        }
        if (s.name != it->path().filename().u8string()) {
            warnings.push_back(md.u8string() + ": names '" + s.name + "', but its folder is '" +
                               it->path().filename().u8string() + "'");
            continue;
        }
        s.source = source;
        s.dir = it->path().u8string();
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const Skill & a, const Skill & b) { return a.name < b.name; });
    return out;
}

} // namespace bmoe::server::agent
