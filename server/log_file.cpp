#include "log_file.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace bmoe::server {

bool rotate_log(const std::string & path, std::uintmax_t max_bytes, std::string & err) {
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    const std::uintmax_t size = fs::file_size(p, ec);
    if (ec || size < max_bytes) return true; // nothing there, or nothing to do
    const fs::path old = fs::u8path(path + ".1");
    fs::remove(old, ec);
    ec.clear();
    fs::rename(p, old, ec);
    if (ec) {
        err = "could not rotate " + path + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace bmoe::server
