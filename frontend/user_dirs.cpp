#include "user_dirs.h"

#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace bmoe::frontend {

std::string default_data_dir() {
#if defined(_WIN32)
    PWSTR w = nullptr;
    std::string base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &w)) && w) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            base.resize((size_t) n - 1);
            WideCharToMultiByte(CP_UTF8, 0, w, -1, base.data(), n, nullptr, nullptr);
        }
    }
    CoTaskMemFree(w);
    return (fs::u8path(base) / "BigMoeOnEdge").u8string();
#elif defined(__APPLE__)
    const char * home = std::getenv("HOME");
    return (fs::path(home ? home : ".") / "Library" / "Application Support" / "BigMoeOnEdge").u8string();
#else
    const char * xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return (fs::path(xdg) / "bigmoeonedge").u8string();
    const char * home = std::getenv("HOME");
    return (fs::path(home ? home : ".") / ".local" / "share" / "bigmoeonedge").u8string();
#endif
}

std::string records_dir(const std::string & data_dir) {
    return data_dir.empty() ? std::string() : (fs::u8path(data_dir) / "routing").u8string();
}

} // namespace bmoe::frontend
