// Where a record was made, as a person would describe it, and the system's own memory counters
// around a run (env_probe.h). The one file of the tool that names platforms: nothing here is read
// by a rule, it is the description a table of runs is grouped and explained by.
//
// Deliberately absent: the machine's model name, its serial, its host name, the user. A record is
// meant to be shared, and what identifies a class of machine (the processor, the memory, the
// system) is not what identifies one.

#include "env_probe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/utsname.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace bmoe {

namespace {

[[maybe_unused]] std::string first_line(const char * path) {
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    return line;
}

// `Key:   123 kB` in a /proc file, as bytes; -1 when absent.
[[maybe_unused]] long long proc_kb(const char * path, const char * key) {
    std::ifstream f(path);
    std::string line;
    const size_t n = std::strlen(key);
    while (std::getline(f, line))
        if (line.compare(0, n, key) == 0 && line.size() > n && line[n] == ':')
            return std::strtoll(line.c_str() + n + 1, nullptr, 10) * 1024;
    return -1;
}

// `key 123` in /proc/vmstat; -1 when absent.
[[maybe_unused]] long long vmstat(const char * key) {
    std::ifstream f("/proc/vmstat");
    std::string k;
    long long v = 0;
    while (f >> k >> v)
        if (k == key) return v;
    return -1;
}

[[maybe_unused]] void put(Facts & f, const std::string & key, long long v) {
    if (v >= 0) set_fact(f, key, v);
}

#if defined(__APPLE__)
std::string sysctl_text(const char * name) {
    char buf[256];
    size_t len = sizeof(buf);
    if (sysctlbyname(name, buf, &len, nullptr, 0) != 0 || len == 0) return "";
    return std::string(buf, strnlen(buf, len));
}
#endif

} // namespace

void probe_environment(Facts & env) {
#if defined(_WIN32)
    env["os"] = "Windows";
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    env["arch"] = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64   ? "arm64"
                  : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x86_64"
                                                                              : "other";
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        // The commit limit is physical memory plus the page files: the difference is the swap.
        set_fact(env, "swap_total_bytes", (long long) (ms.ullTotalPageFile - ms.ullTotalPhys));
        env["swap_kind"] = ms.ullTotalPageFile > ms.ullTotalPhys ? "pagefile" : "none";
    }
#else
    struct utsname u;
    if (uname(&u) == 0) {
        env["os"] = u.sysname;
        env["kernel"] = u.release;
        env["arch"] = u.machine;
    }
#endif

#if defined(__APPLE__)
    env["os_version"] = sysctl_text("kern.osproductversion");
    env["cpu"] = sysctl_text("machdep.cpu.brand_string");
    struct xsw_usage sw;
    size_t len = sizeof(sw);
    if (sysctlbyname("vm.swapusage", &sw, &len, nullptr, 0) == 0) {
        set_fact(env, "swap_total_bytes", (long long) sw.xsu_total);
        // Swap files here are made as they are needed, so the total is what exists now, not a limit.
        env["swap_kind"] = "dynamic-file";
    }
    env["memory_compression"] = "true";
#elif defined(__ANDROID__)
    env["os"] = "Android";
    char prop[PROP_VALUE_MAX];
    if (__system_property_get("ro.build.version.release", prop) > 0) env["os_version"] = prop;
    if (__system_property_get("ro.build.version.sdk", prop) > 0) env["os_api"] = prop;
    // The system-on-chip, which names a class of device; the product's own name is left out.
    if (__system_property_get("ro.soc.model", prop) > 0) env["cpu"] = prop;
#elif defined(__linux__)
    {
        std::ifstream f("/etc/os-release");
        std::string line;
        while (std::getline(f, line))
            if (line.compare(0, 12, "PRETTY_NAME=") == 0) {
                std::string v = line.substr(12);
                if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
                env["os_version"] = v;
            }
    }
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line))
            if (line.compare(0, 10, "model name") == 0) {
                const size_t c = line.find(':');
                if (c != std::string::npos) env["cpu"] = line.substr(line.find_first_not_of(" \t", c + 1));
                break;
            }
    }
#endif

#if defined(__linux__)
    put(env, "swap_total_bytes", proc_kb("/proc/meminfo", "SwapTotal"));
    // What the swap is made of decides what losing memory costs: compressed in RAM, or written out.
    {
        std::ifstream f("/proc/swaps");
        std::string line, kind = "none";
        std::getline(f, line); // the header
        while (std::getline(f, line))
            kind = line.find("zram") != std::string::npos ? (kind == "disk" ? "zram+disk" : "zram")
                                                          : (kind == "zram" ? "zram+disk" : "disk");
        env["swap_kind"] = kind;
    }
    const std::string swappiness = first_line("/proc/sys/vm/swappiness");
    if (!swappiness.empty()) env["swappiness"] = swappiness;
    // A container's own limit, which is what "available" means inside one. "max" is no limit.
    const std::string limit = first_line("/sys/fs/cgroup/memory.max");
    if (!limit.empty()) env["cgroup_memory_max"] = limit;
    const std::string thp = first_line("/sys/kernel/mm/transparent_hugepage/enabled");
    if (!thp.empty()) env["transparent_hugepage"] = thp;
#endif
}

void sample_system(Facts & out) {
#if defined(__APPLE__)
    struct xsw_usage sw;
    size_t len = sizeof(sw);
    if (sysctlbyname("vm.swapusage", &sw, &len, nullptr, 0) == 0)
        set_fact(out, "swap_used_bytes", (long long) sw.xsu_used);
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t) &vm, &count) == KERN_SUCCESS) {
        set_fact(out, "swapins", (long long) vm.swapins);
        set_fact(out, "swapouts", (long long) vm.swapouts);
        set_fact(out, "compressions", (long long) vm.compressions);
        set_fact(out, "decompressions", (long long) vm.decompressions);
        set_fact(out, "pageins", (long long) vm.pageins);
        set_fact(out, "compressor_pages", (long long) vm.compressor_page_count);
        set_fact(out, "free_pages", (long long) vm.free_count);
    }
#elif defined(__linux__)
    const long long total = proc_kb("/proc/meminfo", "SwapTotal"), free_ = proc_kb("/proc/meminfo", "SwapFree");
    if (total >= 0 && free_ >= 0) set_fact(out, "swap_used_bytes", total - free_);
    put(out, "mem_available_bytes", proc_kb("/proc/meminfo", "MemAvailable"));
    put(out, "page_cache_bytes", proc_kb("/proc/meminfo", "Cached"));
    put(out, "swapins", vmstat("pswpin"));
    put(out, "swapouts", vmstat("pswpout"));
    put(out, "major_faults", vmstat("pgmajfault"));
    put(out, "direct_reclaim_stalls", vmstat("allocstall_normal"));
    // The container's own account: what a second load finds already charged to it.
    const std::string current = first_line("/sys/fs/cgroup/memory.current");
    if (!current.empty()) out["cgroup_memory_current"] = current;
    // Pressure as the kernel states it: the share of the last 10 s some task waited for memory.
    const std::string psi = first_line("/proc/pressure/memory");
    const size_t at = psi.find("avg10=");
    if (at != std::string::npos) out["memory_pressure_avg10"] = psi.substr(at + 6, psi.find(' ', at) - at - 6);
#elif defined(_WIN32)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        set_fact(out, "mem_available_bytes", (long long) ms.ullAvailPhys);
        set_fact(out, "commit_available_bytes", (long long) ms.ullAvailPageFile);
    }
#else
    (void) out;
#endif
}

} // namespace bmoe
