#include "model_delete.h"

#include "model_files.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace bmoe::server {

namespace {

// One part of a listed name: no separator, no drive colon, nothing that names a folder.
bool plain_part(const std::string & s) {
    return !s.empty() && s != "." && s != ".." && s.find_first_of("/\\:") == std::string::npos &&
           s.find('\0') == std::string::npos;
}

} // namespace

bool find_model_to_delete(
    const std::string & models_dir, const std::string & rel, ModelDeletion & out, bool & not_found, std::string & err) {
    out = {};
    not_found = false;
    const size_t slash = rel.find('/');
    const std::string folder = slash == std::string::npos ? "" : rel.substr(0, slash);
    const std::string name = slash == std::string::npos ? rel : rel.substr(slash + 1);
    if (models_dir.empty() || !plain_part(name) || (slash != std::string::npos && !plain_part(folder))) {
        err = "a model is named as the listing names it: its file, or its folder and file";
        return false;
    }
    for (const ModelFiles & m : scan_model_files(models_dir)) {
        if (m.folder != folder || m.first != name) continue;
        const fs::path base = folder.empty() ? fs::u8path(models_dir) : fs::u8path(models_dir) / fs::u8path(folder);
        out.folder = folder;
        for (const std::string & f : m.files) {
            out.names.push_back(f);
            out.paths.push_back((base / fs::u8path(f)).u8string());
        }
        return true;
    }
    not_found = true;
    err = "no model " + rel + " in the models folder";
    return false;
}

bool deletion_touches(const ModelDeletion & d, const std::string & loaded) {
    if (loaded.empty()) return false;
    for (const std::string & p : d.paths) {
        std::error_code ec;
        if (fs::equivalent(fs::u8path(p), fs::u8path(loaded), ec) && !ec) return true;
    }
    return false;
}

bool delete_model_files(const std::string & models_dir, const ModelDeletion & d, uint64_t & freed, std::string & err) {
    freed = 0;
    for (const std::string & p : d.paths) {
        const fs::path path = fs::u8path(p);
        std::error_code ec;
        // symlink_status: a link is removed as the link, never followed to what it points at.
        const fs::file_status st = fs::symlink_status(path, ec);
        if (ec || !fs::exists(st)) continue;
        if (fs::is_directory(st)) {
            err = path.filename().u8string() + " is a directory, not a model file";
            return false;
        }
        std::error_code size_ec;
        const auto size = fs::is_regular_file(st) ? fs::file_size(path, size_ec) : 0;
        if (!fs::remove(path, ec) || ec) {
            err = "could not delete " + path.filename().u8string() + (ec ? ": " + ec.message() : "");
            return false;
        }
        freed += size_ec ? 0 : (uint64_t) size;
    }
    // The folder a split set was downloaded into goes with it, but only once nothing else is in
    // it: remove() refuses a folder that is not empty, which is the check.
    if (!d.folder.empty()) {
        std::error_code ec;
        const fs::path folder = fs::u8path(models_dir) / fs::u8path(d.folder);
        if (!fs::is_symlink(folder, ec)) fs::remove(folder, ec);
    }
    return true;
}

} // namespace bmoe::server
