#include "platform.h"

#include <cstdlib>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <climits>
#else
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <climits>
#endif

namespace fs = std::filesystem;

namespace bmoe::server::platform {

namespace {

#if defined(_WIN32)
std::string narrow(const std::wstring & w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string & s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
    return w;
}
#endif

} // namespace

json host_info() {
    unsigned long long total = 0, avail = 0;
#if defined(_WIN32)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        total = ms.ullTotalPhys;
        avail = ms.ullAvailPhys;
    }
#elif defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) total = mem;
    vm_statistics64_data_t vs;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t) &vs, &count) == KERN_SUCCESS)
        avail = (unsigned long long) (vs.free_count + vs.inactive_count) * (unsigned long long) vm_page_size;
#else
    std::ifstream in("/proc/meminfo");
    std::string key;
    unsigned long long kb = 0;
    std::string unit;
    while (in >> key >> kb >> unit) {
        if (key == "MemTotal:") total = kb * 1024ull;
        if (key == "MemAvailable:") avail = kb * 1024ull;
    }
#endif
    return json{{"ram_total_mib", total >> 20},
                {"ram_available_mib", avail >> 20},
                {"cpu_threads", std::thread::hardware_concurrency()}};
}

std::string executable_dir() {
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD) buf.size());
        if (n == 0) return "";
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path().u8string();
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return "";
    std::error_code ec;
    return fs::canonical(buf, ec).parent_path().u8string();
#else
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? "" : p.parent_path().u8string();
#endif
}

std::string default_data_dir() {
#if defined(_WIN32)
    PWSTR w = nullptr;
    std::string base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &w))) base = narrow(w);
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

void open_url(const std::string & url) {
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::system(("open '" + url + "'").c_str());
#else
    std::system(("xdg-open '" + url + "' >/dev/null 2>&1 &").c_str());
#endif
}

bool started_by_double_click() {
#if defined(_WIN32)
    // A terminal the user already had open also holds the console; one Explorer created for us
    // holds only us.
    DWORD pid;
    return GetConsoleProcessList(&pid, 1) == 1;
#else
    return false;
#endif
}

} // namespace bmoe::server::platform
