#include "download_policy.h"

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace bmoe::server {

bool is_transient_curl_exit(int rc) {
    switch (rc) {
    case 6:  // could not resolve host: the line is down, or DNS has not come back with it
    case 7:  // could not connect
    case 16: // HTTP/2 framing error
    case 18: // partial file: the body ended before the length it announced
    case 28: // operation timed out
    case 35: // TLS handshake failed
    case 52: // the server answered nothing
    case 55: // failure sending
    case 56: // failure receiving: the connection was reset mid-transfer
    case 92: // HTTP/2 stream error
        return true;
    default:
        return false;
    }
}

std::chrono::seconds download_retry_delay(int failed) {
    std::chrono::seconds d = kDownloadRetryFirstDelay;
    for (int i = 1; i < failed && d < kDownloadRetryMaxDelay; ++i)
        d *= 2;
    return d < kDownloadRetryMaxDelay ? d : kDownloadRetryMaxDelay;
}

uint64_t bytes_still_needed(uint64_t expected, uint64_t on_disk) {
    return expected > on_disk ? expected - on_disk : 0;
}

std::string gb_label(uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f GB", (double) bytes / 1e9);
    return buf;
}

std::string free_space_refusal(uint64_t needed, uint64_t available) {
    if (available == 0 || needed <= available) return "";
    return "needs " + gb_label(needed) + " more, only " + gb_label(available) + " free";
}

bool partial_file_path(const std::string & models_dir, const std::string & name, std::string & out) {
    if (models_dir.empty() || name.empty() || name == "." || name == ".." ||
        name.find_first_of("/\\") != std::string::npos || name.find(':') != std::string::npos ||
        name.find('\0') != std::string::npos)
        return false;
    // A plain name joined to the folder cannot leave it; the check below holds that against a
    // filesystem that reads a name differently (a drive-relative one on Windows).
    const fs::path dir = fs::u8path(models_dir);
    const fs::path p = dir / fs::u8path(name + ".part");
    if (p.parent_path() != dir || p.filename().u8string() != name + ".part") return false;
    out = p.u8string();
    return true;
}

bool delete_partial_files(const std::string & models_dir,
                          const std::vector<std::string> & names,
                          uint64_t & freed,
                          std::string & err) {
    freed = 0;
    std::vector<fs::path> parts;
    for (const std::string & n : names) {
        std::string p;
        if (!partial_file_path(models_dir, n, p)) {
            err = "'" + n + "' is not a plain file name";
            return false;
        }
        parts.push_back(fs::u8path(p));
    }
    for (const fs::path & p : parts) {
        std::error_code ec;
        // symlink_status: a link named like a part is removed as the link, never followed.
        const fs::file_status st = fs::symlink_status(p, ec);
        if (ec || !fs::exists(st)) continue;
        if (fs::is_directory(st)) {
            err = p.filename().u8string() + " is a directory, not a partial download";
            return false;
        }
        std::error_code size_ec;
        const auto size = fs::is_regular_file(st) ? fs::file_size(p, size_ec) : 0;
        const uint64_t n = size_ec ? 0 : (uint64_t) size;
        if (!fs::remove(p, ec) || ec) {
            err = "could not delete " + p.filename().u8string() + (ec ? ": " + ec.message() : "");
            return false;
        }
        freed += n;
    }
    return true;
}

} // namespace bmoe::server
