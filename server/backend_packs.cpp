#include "backend_packs.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace bmoe::server {

namespace {

// tar lists "./a" and "a" for the same entry depending on how the archive was made, and the root
// itself as "./" or ".".
std::string strip_dot_slash(std::string name) {
    while (name.compare(0, 2, "./") == 0)
        name.erase(0, 2);
    return name;
}

std::string without_trailing_slash(std::string name) {
    while (!name.empty() && name.back() == '/')
        name.pop_back();
    return name;
}

} // namespace

bool pack_applies(const FeedPack & pack, const LibraryProbe & loads) {
    if (!loads) return false;
    for (const std::string & lib : pack.needs_any)
        if (loads(lib)) return true;
    return false;
}

std::vector<std::string> backend_pack_dirs(const std::string & backends_root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(backends_root), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code tec;
        // A link to a folder elsewhere is not a pack this server installed.
        if (!it->is_directory(tec) || it->is_symlink(tec)) continue;
        out.push_back(it->path().u8string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool read_pack_marker(const std::string & pack_dir, InstalledPack & out) {
    std::ifstream in(fs::u8path(pack_dir) / kPackMarker, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        const json j = json::parse(ss.str());
        if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) return false;
        out.id = j["id"].get<std::string>();
        out.version = j.value("version", "");
        out.sha256 = j.value("sha256", "");
    } catch (const std::exception &) {
        return false;
    }
    return is_pack_id(out.id);
}

bool write_pack_marker(const std::string & pack_dir, const InstalledPack & pack, std::string & err) {
    std::ofstream out(fs::u8path(pack_dir) / kPackMarker, std::ios::binary | std::ios::trunc);
    out << json{{"id", pack.id}, {"version", pack.version}, {"sha256", pack.sha256}}.dump(2) << "\n";
    if (!out) {
        err = "could not write the pack's " + std::string(kPackMarker);
        return false;
    }
    return true;
}

std::vector<InstalledPack> installed_packs(const std::string & backends_root) {
    std::vector<InstalledPack> out;
    for (const std::string & dir : backend_pack_dirs(backends_root)) {
        InstalledPack p;
        // The folder is named after the pack it holds; a marker that says otherwise is not believed.
        if (read_pack_marker(dir, p) && p.id == fs::u8path(dir).filename().u8string()) out.push_back(p);
    }
    return out;
}

std::string archive_entry_refusal(const std::string & raw) {
    if (raw.empty()) return "an entry with no name";
    if (raw.find('\0') != std::string::npos) return "an entry whose name holds a NUL";
    if (raw[0] == '/') return "'" + raw + "' is an absolute path";
    if (raw.find('\\') != std::string::npos) return "'" + raw + "' holds a backslash";
    if (raw.find(':') != std::string::npos) return "'" + raw + "' names a drive or a stream";
    const std::string name = strip_dot_slash(raw);
    size_t i = 0;
    while (i <= name.size()) {
        const size_t j = name.find('/', i);
        const std::string seg = name.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (seg == "..") return "'" + raw + "' leaves the folder through '..'";
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return "";
}

std::string archive_listing_refusal(const std::vector<std::string> & names, const std::vector<std::string> & verbose) {
    if (names.empty()) return "the archive is empty";
    if (names.size() != verbose.size()) return "tar's two listings of the archive disagree";
    std::vector<std::string> links;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string why = archive_entry_refusal(names[i]);
        if (!why.empty()) return why;
        const std::string & line = verbose[i];
        const char type = line.empty() ? '?' : line[0];
        // The verbose listing's first column is the entry's mode, its first letter the type, as
        // both GNU tar and bsdtar print it. A hard link reads as a file in some tars and says
        // " link to " after its name in all of them.
        if (line.find(" link to ") != std::string::npos) return "'" + names[i] + "' is a hard link";
        if (type == 'l') {
            links.push_back(without_trailing_slash(strip_dot_slash(names[i])));
            continue;
        }
        if (type != '-' && type != 'd') return "'" + names[i] + "' is neither a file, a folder nor a link";
    }
    for (const std::string & link : links)
        for (const std::string & n : names) {
            const std::string name = strip_dot_slash(n);
            if (name.size() > link.size() && name.compare(0, link.size(), link) == 0 && name[link.size()] == '/')
                return "'" + n + "' lies under the link '" + link + "'";
        }
    return "";
}

std::string unpacked_tree_refusal(const std::string & dir) {
    std::error_code ec;
    const fs::path root = fs::u8path(dir);
    for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::file_status st = it->symlink_status(ec);
        if (ec) return "could not read " + it->path().u8string();
        if (fs::is_symlink(st)) {
            const fs::path target = fs::read_symlink(it->path(), ec);
            if (ec) return "could not read the link " + it->path().u8string();
            if (target.is_absolute() || target.has_root_name() || target.has_root_directory())
                return "the link " + it->path().filename().u8string() + " points outside the pack";
            const fs::path rel = (it->path().parent_path() / target).lexically_normal().lexically_relative(root);
            if (rel.empty() || *rel.begin() == "..")
                return "the link " + it->path().filename().u8string() + " points outside the pack";
            continue;
        }
        if (!fs::is_regular_file(st) && !fs::is_directory(st))
            return it->path().filename().u8string() + " is neither a file, a folder nor a link";
    }
    if (ec) return "could not read the unpacked pack: " + ec.message();
    return "";
}

json backends_answer(const std::vector<std::string> & registered,
                     const std::string & target,
                     const Feed * feed,
                     const std::vector<InstalledPack> & installed,
                     const LibraryProbe & loads,
                     bool restart_required) {
    json packs = json::array();
    std::set<std::string> offered;
    if (feed)
        for (const FeedPack & p : feed->packs) {
            if (p.target != target) continue;
            offered.insert(p.id);
            json row{{"id", p.id},           {"title", p.title},
                     {"size", p.asset.size}, {"applies", pack_applies(p, loads)},
                     {"installed", false},   {"installed_version", nullptr},
                     {"current", false}};
            for (const InstalledPack & i : installed)
                if (i.id == p.id) {
                    row["installed"] = true;
                    row["installed_version"] = i.version;
                    row["current"] = i.sha256 == p.asset.sha256;
                }
            packs.push_back(row);
        }
    json inst = json::array();
    for (const InstalledPack & i : installed)
        inst.push_back(json{{"id", i.id}, {"version", i.version}, {"offered", offered.count(i.id) > 0}});
    return json{{"registered", registered},
                {"target", target},
                {"feed", feed ? json{{"version", feed->version}, {"published", feed->published}} : json()},
                {"packs", packs},
                {"installed", inst},
                {"restart_required", restart_required}};
}

} // namespace bmoe::server
