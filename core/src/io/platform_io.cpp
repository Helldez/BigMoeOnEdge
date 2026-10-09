#include "platform_io.h"

#include <atomic>

// System headers MUST be included at global scope, never inside the namespace below:
// <cstdlib> etc. do `using ::abs;` and would otherwise be pulled into bmoe::pio, where
// ::abs is not visible (GCC hard-errors; MSVC happened to tolerate it).
#if defined(_WIN32)
#include <windows.h>
#include <malloc.h>
#include <cstring>
#else
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <chrono>
#include <ctime>
#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0 // absent on some BSDs; the mapping is simply charged as usual there
#endif
#if defined(__ANDROID__)
#include <android/hardware_buffer.h> // reclaim-exempt allocation; see pinned_alloc
#endif
#if defined(__APPLE__)
#include <libproc.h>     // this process's own regions; see file_mapped_regions
#include <mach-o/dyld.h> // this executable's own path; see run_self
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#endif

namespace bmoe::pio {

#if !defined(_WIN32)
// mincore's vector argument is `unsigned char *` on Linux/Android but `char *` on the BSDs and
// macOS. Name the difference once instead of casting blind at the call site.
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
using mincore_vec_t = char;
#else
using mincore_vec_t = unsigned char;
#endif
#endif

#if defined(_WIN32)

const fd_t fd_invalid = (void *) INVALID_HANDLE_VALUE;
bool fd_ok(fd_t fd) {
    return fd != (void *) INVALID_HANDLE_VALUE;
}

fd_t open_read(const char * path, bool direct, bool * effective_direct) {
    DWORD flags = FILE_ATTRIBUTE_NORMAL | (direct ? FILE_FLAG_NO_BUFFERING : 0);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
    if (effective_direct) *effective_direct = direct && fd_ok((fd_t) h);
    return (fd_t) h;
}

void close_fd(fd_t fd) {
    if (fd_ok(fd)) CloseHandle((HANDLE) fd);
}

long long pread_at(fd_t fd, void * buf, size_t count, uint64_t off) {
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset = (DWORD) (off & 0xFFFFFFFFull);
    ov.OffsetHigh = (DWORD) (off >> 32);
    // FILE_FLAG_NO_BUFFERING rejects a length that is not a multiple of the sector
    // size, so cap per-call length to a sector-aligned 1 GiB chunk (0x7FFFFFFF is odd).
    DWORD to_read = count > 0x40000000ull ? 0x40000000ul : (DWORD) count;
    DWORD got = 0;
    if (!ReadFile((HANDLE) fd, buf, to_read, &got, &ov)) {
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return (long long) got;
}

uint64_t file_size(fd_t fd) {
    LARGE_INTEGER sz;
    return GetFileSizeEx((HANDLE) fd, &sz) ? (uint64_t) sz.QuadPart : 0;
}

void * alloc_aligned(size_t align, size_t sz) {
    return _aligned_malloc(sz, align);
}
void aligned_free(void * p) {
    if (p) _aligned_free(p);
}

size_t vm_page() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t) si.dwPageSize;
}
void * vm_reserve(size_t sz) {
    return VirtualAlloc(nullptr, sz, MEM_RESERVE, PAGE_READWRITE);
}
bool vm_commit(void * p, size_t sz) {
    return VirtualAlloc(p, sz, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}
bool vm_pin(void * /*p*/, size_t /*sz*/) {
    return false; // VirtualLock is bounded by the working-set quota, which is not ours to raise
}
void vm_evict(void * p, size_t sz) {
    if (sz) VirtualFree(p, sz, MEM_DECOMMIT);
}
void vm_release(void * p, size_t /*sz*/) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}
void vm_drop_file_pages(void * /*p*/, size_t /*sz*/) {
    // File-backed views cannot be decommitted (MEM_DECOMMIT is only valid for VirtualAlloc'd pages),
    // and the host build never mmaps the model for streaming — so there is nothing to drop.
}
void vm_drop_anon_pages(void * /*p*/, size_t /*sz*/) {
    // MEM_DECOMMIT would turn the owner's next write into an access violation, and MEM_RESET is only
    // defined for VirtualAlloc'd regions, which a heap allocation need not be. The host build has no
    // prefill device that moves state away, so keeping the pages costs nothing here.
}

void vm_advise_random(void * /*p*/, size_t /*sz*/) {
    // No readahead to tame on a host build that never streams; the gates do not measure I/O.
}

// Unmeasured on the host build, like the fault counters below and for the same reason: the gates
// prove byte-identity, they do not size a cache against a phone's reclaim. QueryWorkingSetEx could
// answer this, but nothing here consumes it.
bool vm_resident_sample(const void * /*p*/, size_t /*sz*/, size_t * /*sampled*/, size_t * /*resident*/) {
    return false;
}

bool file_mapped_regions(const char * /*basename*/, std::vector<MappedRegion> & /*out*/) {
    return false; // no /proc/self/maps; the host gates do not exercise dense residency
}

uint64_t mem_available_bytes() {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    return GlobalMemoryStatusEx(&ms) ? (uint64_t) ms.ullAvailPhys : 0;
}

uint64_t mem_total_bytes() {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    return GlobalMemoryStatusEx(&ms) ? (uint64_t) ms.ullTotalPhys : 0;
}

// The host build exists for the byte-identity gates, not perf measurement, so these stay
// unmeasured (0) rather than pulling in the imperfect Windows equivalents (PageFaultCount counts
// soft faults too; GetProcessTimes would work but there is no consumer for it here).
uint64_t major_faults() {
    return 0;
}
double process_cpu_seconds() {
    return 0.0;
}

size_t fault_bytes() {
    return vm_page();
}

bool process_memory(ProcessMemory * /*out*/) {
    return false;
}

bool device_memory(DeviceMemory * out) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return false;
    out->available_bytes = (uint64_t) ms.ullAvailPhys;
    out->free_bytes = (uint64_t) ms.ullAvailPhys; // no separate "free vs reclaimable" here
    out->swap_free_bytes = (uint64_t) ms.ullAvailPageFile;
    return true;
}

#else

const fd_t fd_invalid = -1;
bool fd_ok(fd_t fd) {
    return fd >= 0;
}

fd_t open_read(const char * path, bool direct, bool * effective_direct) {
#if defined(__APPLE__)
    // No O_DIRECT here (the shim above leaves it 0), so a direct request is an ordinary open plus
    // F_NOCACHE: ask the kernel not to keep this descriptor's pages. That is the whole of Apple's
    // uncached mode — a caching hint on the fd, not an I/O mode — so a refusal is a downgrade to
    // buffered for the caller to report, never a reason to fail a perfectly good descriptor.
    const fd_t fd = open(path, O_RDONLY | O_CLOEXEC);
    const bool ok = fd_ok(fd) && direct && fcntl(fd, F_NOCACHE, 1) == 0;
    if (effective_direct) *effective_direct = ok;
    return fd;
#else
    const fd_t fd = open(path, O_RDONLY | O_CLOEXEC | (direct ? O_DIRECT : 0));
    if (effective_direct) *effective_direct = direct && fd_ok(fd);
    return fd;
#endif
}

void close_fd(fd_t fd) {
    if (fd_ok(fd)) close(fd);
}

long long pread_at(fd_t fd, void * buf, size_t count, uint64_t off) {
    return (long long) pread(fd, buf, count, (off_t) off);
}

uint64_t file_size(fd_t fd) {
    // fstat rather than a seek pair: every consumer reads with pread, so the fd's file position is
    // never used — and mutating shared fd state to answer a question about the file is a trap waiting
    // for the first caller that does rely on the position.
    struct stat st;
    return fstat(fd, &st) == 0 && st.st_size > 0 ? (uint64_t) st.st_size : 0;
}

void * alloc_aligned(size_t align, size_t sz) {
    void * p = nullptr;
    return posix_memalign(&p, align, sz) == 0 ? p : nullptr;
}
void aligned_free(void * p) {
    free(p);
}

size_t vm_page() {
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (size_t) ps : 4096;
}
void * vm_reserve(size_t sz) {
    // MAP_NORESERVE makes the "address-only" contract true rather than merely true-by-default: these
    // spans are reserved at full expert-set size but only ever committed for resident slices, so the
    // untouched remainder must not be charged against the commit limit (which is what strict
    // overcommit would do, refusing a reservation the cache never intends to fill).
    void * p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
bool vm_commit(void * /*p*/, size_t /*sz*/) {
    return true; // POSIX commits on first touch
}
namespace {
std::atomic<bool> g_pinned_any{false};
}
bool vm_pin(void * p, size_t sz) {
    if (!sz) return true;
    if (mlock(p, sz) != 0) return false;
    g_pinned_any.store(true, std::memory_order_relaxed);
    return true;
}
void vm_evict(void * p, size_t sz) {
    if (!sz) return;
#if defined(__APPLE__)
    // Darwin's MADV_DONTNEED is advice and frees nothing: the pages of an evicted expert stay
    // dirty and anonymous, so the process grows towards the whole expert set whatever the cache
    // budget says, and the kernel answers by compressing it - the live cache included. Every hit
    // then pays a decompression that no fault counter shows. Measured on a 16 GB machine with a
    // 9 GB budget: 8.4 GB of this process in the compressor and decode at a third of its rate.
    // Mapping fresh zero-fill pages over the span is the release that cannot be declined; the
    // address stays valid, which is the contract.
    if (mmap(p, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) !=
        MAP_FAILED)
        return;
    madvise(p, sz, MADV_FREE); // lazily, if the remap was refused
#else
    // A locked range refuses MADV_DONTNEED, so a pinned slice is unlocked before it is dropped.
    if (g_pinned_any.load(std::memory_order_relaxed)) munlock(p, sz);
    madvise(p, sz, MADV_DONTNEED);
#endif
}
void vm_release(void * p, size_t sz) {
    if (p) munmap(p, sz);
}
void vm_drop_file_pages(void * p, size_t sz) {
    // MADV_DONTNEED on the model's clean, read-only MAP_PRIVATE mapping drops the resident pages; the
    // next access refaults them from the file. The tensor was rebound onto its anon copy, so nothing
    // touches this range again — the drop just reclaims the double residency, it does not lose data.
    if (sz) madvise(p, sz, MADV_DONTNEED);
}
void vm_drop_anon_pages(void * p, size_t sz) {
    // On private anonymous memory MADV_DONTNEED frees the pages and leaves the range mapped: a later
    // read sees zeros, a later write faults in a fresh page. Advice only; a failure keeps the pages.
    if (sz) madvise(p, sz, MADV_DONTNEED);
}

void vm_advise_random(void * p, size_t sz) {
    // MADV_RANDOM disables readahead for the range: each fault maps exactly the page that faulted.
    // Advice only, so a failure changes nothing but the readahead and is not worth reporting.
    if (sz) madvise(p, sz, MADV_RANDOM);
}

bool vm_resident_sample(const void * p, size_t sz, size_t * sampled, size_t * resident) {
    if (!p || sz == 0) return true; // an empty range is measured, and holds nothing
    const size_t page = vm_page();
    // Clip to the pages FULLY inside the range, matching how the eviction path releases them: an
    // edge page shared with a neighbouring slice belongs to that neighbour, not to this sample.
    uintptr_t a0 = ((uintptr_t) p + page - 1) & ~(uintptr_t) (page - 1);
    uintptr_t a1 = ((uintptr_t) p + sz) & ~(uintptr_t) (page - 1);
    if (a1 <= a0) return true;
    unsigned char vec[512]; // one byte per page: 512 pages (2 MiB at 4 KiB pages) per syscall
    for (uintptr_t a = a0; a < a1;) {
        const size_t want = std::min<size_t>((size_t) (a1 - a) / page, sizeof(vec));
        if (mincore((void *) a, want * page, (mincore_vec_t *) vec) != 0) return false;
        for (size_t i = 0; i < want; ++i)
            *resident += (size_t) (vec[i] & 1u); // bit 0 = the page is resident
        *sampled += want;
        a += want * page;
    }
    return true;
}

#if defined(__APPLE__)
namespace {
// Darwin's counterpart of /proc/meminfo is the host's own page accounting.
bool host_vm(vm_statistics64_data_t * vm) {
    static const mach_port_t host = mach_host_self(); // a send right; taken once, not per call
    mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
    return host_statistics64(host, HOST_VM_INFO64, (host_info64_t) vm, &n) == KERN_SUCCESS;
}
// Pages nobody is using, as the kernel counts them: free_count includes the speculative read-ahead
// pages, which are file-backed and counted again under external.
uint64_t host_free_pages(const vm_statistics64_data_t & vm) {
    return vm.free_count > vm.speculative_count ? vm.free_count - vm.speculative_count : 0;
}
// The same definition as MemAvailable, built from the parts Darwin publishes: what can be handed
// out without compressing anything — free pages, file-backed pages (dropped, not compressed) and
// purgeable ones. It inherits MemAvailable's blind spot on purpose, so the two stay comparable: a
// mapped model's resident pages are file-backed and count as available here too.
uint64_t host_available_pages(const vm_statistics64_data_t & vm) {
    return host_free_pages(vm) + vm.external_page_count + vm.purgeable_count;
}
} // namespace
#endif

uint64_t mem_total_bytes() {
    if (FILE * f = std::fopen("/proc/meminfo", "re")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long long kb = 0;
            if (std::sscanf(line, "MemTotal: %llu kB", &kb) == 1) {
                std::fclose(f);
                return (uint64_t) kb * 1024ull;
            }
        }
        std::fclose(f);
    }
#if defined(_SC_PHYS_PAGES)
    const long total = sysconf(_SC_PHYS_PAGES);
    const long psz = sysconf(_SC_PAGESIZE);
    if (total > 0 && psz > 0) return (uint64_t) total * (uint64_t) psz;
#endif
    return 0;
}

uint64_t mem_available_bytes() {
#if defined(__APPLE__)
    vm_statistics64_data_t vm;
    if (host_vm(&vm)) return host_available_pages(vm) * (uint64_t) vm_page();
#endif
    // Linux/Android: MemAvailable is the kernel's own estimate of what can be allocated without
    // swapping (it accounts for reclaimable page cache), which is exactly the sizing signal we want.
    if (FILE * f = std::fopen("/proc/meminfo", "re")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long long kb = 0;
            if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
                std::fclose(f);
                return (uint64_t) kb * 1024ull;
            }
        }
        std::fclose(f);
    }
    // Fallback where /proc is absent (the BSDs): free physical pages. An underestimate — it omits
    // reclaimable cache — but non-zero and safe to size a cache against.
#if defined(_SC_AVPHYS_PAGES)
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    const long ps = sysconf(_SC_PAGESIZE);
    if (pages > 0 && ps > 0) return (uint64_t) pages * (uint64_t) ps;
#endif
    return 0;
}

uint64_t major_faults() {
    // ru_majflt counts faults that required a backing-store read (the ones that stall on flash).
    // RUSAGE_SELF aggregates every thread of the process, matching the multi-threaded decode.
    struct rusage ru;
    return getrusage(RUSAGE_SELF, &ru) == 0 ? (uint64_t) ru.ru_majflt : 0;
}

double process_cpu_seconds() {
    // Total CPU consumed across all threads. Divided by wall×threads downstream, this is the
    // occupancy signal that tells a frequency cap / preemption apart from genuine heavy compute.
#if defined(CLOCK_PROCESS_CPUTIME_ID)
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) return (double) ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
    return 0.0;
}

size_t fault_bytes() {
    // One major fault brings back one page. The kernel can fault a cluster in around a single miss,
    // so this is the floor of what a fault moved, not an upper bound — it under-reports rather than
    // inventing, which is the right direction for a number a reader will compare against real reads.
    return vm_page();
}

// Scan a "Key: <n> kB" file for the keys we want in one pass, rather than one open per field.
namespace {
bool scan_kb_file(const char * path, const char * const * keys, uint64_t * out, int n) {
    FILE * f = std::fopen(path, "re");
    if (!f) return false;
    char line[256];
    int found = 0;
    while (found < n && std::fgets(line, sizeof(line), f)) {
        for (int i = 0; i < n; ++i) {
            if (out[i]) continue; // already have it
            const size_t klen = std::strlen(keys[i]);
            if (std::strncmp(line, keys[i], klen) != 0 || line[klen] != ':') continue;
            unsigned long long kb = 0;
            if (std::sscanf(line + klen + 1, " %llu kB", &kb) == 1) {
                out[i] = (uint64_t) kb * 1024ull;
                ++found;
            }
            break;
        }
    }
    std::fclose(f);
    return found > 0;
}
} // namespace

#if defined(__APPLE__)
// The task's own ledger carries the same split /proc/self/status does: internal pages are the
// anonymous ones, external the file-backed ones, and what the compressor holds is the anonymous
// memory already taken back — the role zram plays in VmSwap.
bool process_memory(ProcessMemory * out) {
    task_vm_info_data_t ti;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &ti, &n) != KERN_SUCCESS) return false;
    out->rss_bytes = ti.resident_size;
    out->rss_anon_bytes = ti.internal;
    out->rss_file_bytes = ti.external;
    out->swap_bytes = ti.compressed;
    return true;
}

bool device_memory(DeviceMemory * out) {
    vm_statistics64_data_t vm;
    if (!host_vm(&vm)) return false;
    out->available_bytes = host_available_pages(vm) * (uint64_t) vm_page();
    out->free_bytes = host_free_pages(vm) * (uint64_t) vm_page();
    // Swap files are created on demand, so "free" is what is left of the ones that exist now.
    struct xsw_usage sw;
    size_t len = sizeof(sw);
    out->swap_free_bytes = sysctlbyname("vm.swapusage", &sw, &len, nullptr, 0) == 0 ? sw.xsu_avail : 0;
    return true;
}
#else
bool process_memory(ProcessMemory * out) {
    static const char * const keys[] = {"VmRSS", "RssAnon", "RssFile", "VmSwap"};
    uint64_t v[4] = {0, 0, 0, 0};
    if (!scan_kb_file("/proc/self/status", keys, v, 4)) return false;
    out->rss_bytes = v[0];
    out->rss_anon_bytes = v[1];
    out->rss_file_bytes = v[2];
    out->swap_bytes = v[3];
    return true;
}

bool device_memory(DeviceMemory * out) {
    static const char * const keys[] = {"MemAvailable", "MemFree", "SwapFree"};
    uint64_t v[3] = {0, 0, 0};
    if (!scan_kb_file("/proc/meminfo", keys, v, 3)) return false;
    out->available_bytes = v[0];
    out->free_bytes = v[1];
    out->swap_free_bytes = v[2];
    return true;
}
#endif

#if defined(__APPLE__)
// Darwin has no /proc. The same question — which of this process's regions map this file, and from
// which file offset — is answered by walking its own address space: PROC_PIDREGIONPATHINFO returns
// the region containing or following an address together with the vnode path behind it, and asking
// about oneself needs no entitlement.
bool file_mapped_regions(const char * basename, std::vector<MappedRegion> & out) {
    const size_t blen = std::strlen(basename);
    const pid_t pid = getpid();
    bool any = false;
    uint64_t addr = 0;
    for (;;) {
        struct proc_regionwithpathinfo info;
        if (proc_pidinfo(pid, PROC_PIDREGIONPATHINFO, addr, &info, sizeof(info)) != (int) sizeof(info)) break;
        const uint64_t start = info.prp_prinfo.pri_address;
        const uint64_t size = info.prp_prinfo.pri_size;
        if (size == 0 || start + size <= addr) break; // no forward progress: the walk is over
        addr = start + size;
        const char * path = info.prp_vip.vip_path;
        const size_t plen = strnlen(path, sizeof(info.prp_vip.vip_path));
        if (plen < blen || std::strncmp(path + plen - blen, basename, blen) != 0) continue;
        out.push_back({(uintptr_t) start, (uintptr_t) (start + size), info.prp_prinfo.pri_offset});
        any = true;
    }
    return any;
}
#else
bool file_mapped_regions(const char * basename, std::vector<MappedRegion> & out) {
    FILE * f = std::fopen("/proc/self/maps", "re");
    if (!f) return false;
    const size_t blen = std::strlen(basename);
    char line[512];
    bool any = false;
    // Each line: "start-end perms offset dev inode   pathname". We want the VMAs whose pathname ends
    // with the model's file name — an mmap of a large file appears as one or more such VMAs.
    while (std::fgets(line, sizeof(line), f)) {
        unsigned long long start = 0, end = 0, off = 0;
        // The pathname is the last field; sscanf %n gives us where the fixed part ended so we can
        // scan the remainder for it without copying.
        int consumed = 0;
        if (std::sscanf(line, "%llx-%llx %*s %llx %*s %*s %n", &start, &end, &off, &consumed) < 3) continue;
        const char * path = line + consumed;
        // Trim the trailing newline and any leading spaces %n may have left.
        while (*path == ' ')
            ++path;
        size_t plen = std::strlen(path);
        while (plen && (path[plen - 1] == '\n' || path[plen - 1] == ' '))
            --plen;
        if (plen < blen) continue;
        if (std::strncmp(path + plen - blen, basename, blen) != 0) continue;
        out.push_back({(uintptr_t) start, (uintptr_t) end, (uint64_t) off});
        any = true;
    }
    std::fclose(f);
    return any;
}
#endif

#endif

// ── A second process, for work that can end one ─────────────────────────────────────────
#if defined(_WIN32)

bool can_run_self() {
    return false; // not implemented here: a caller does the work in its own process
}
ChildResult run_self(const std::vector<std::string> &, double, std::string *) {
    return ChildResult{};
}

#else

} // namespace bmoe::pio
extern char ** environ;
namespace bmoe::pio {

namespace {
// The path the kernel started this process from. Not argv[0], which is whatever the caller typed.
std::string self_path() {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t n = sizeof(buf);
    if (_NSGetExecutablePath(buf, &n) != 0) return "";
    return buf;
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = 0;
    return buf;
#endif
}
} // namespace

bool can_run_self() {
    return !self_path().empty();
}

ChildResult run_self(const std::vector<std::string> & args, double timeout_seconds, std::string * out) {
    ChildResult r;
    const std::string path = self_path();
    int fds[2];
    if (path.empty() || pipe(fds) != 0) return r;

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(path.c_str()));
    for (const std::string & a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, path.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return r;
    }

    // Read until the child closes its end or its time is up. Only the tail is kept: the line a
    // caller wants is the last thing a well-behaved child prints, and a backend can be chatty.
    const size_t keep = 1u << 20;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_seconds);
    bool timed_out = false;
    for (;;) {
        const double left = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0.0) {
            timed_out = true;
            break;
        }
        struct pollfd pf {
            fds[0], POLLIN, 0
        };
        const int pr = poll(&pf, 1, (int) std::min(left * 1000.0, 1000.0));
        if (pr < 0) break;
        if (pr == 0) continue;
        char buf[8192];
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0) break; // the child closed its end
        if (out) {
            out->append(buf, (size_t) n);
            if (out->size() > 2 * keep) out->erase(0, out->size() - keep);
        }
    }
    close(fds[0]);
    if (timed_out) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timed_out) {
        r.outcome = ChildOutcome::TimedOut;
    } else if (WIFSIGNALED(status)) {
        r.outcome = ChildOutcome::Signalled;
        r.code = WTERMSIG(status);
    } else {
        r.outcome = ChildOutcome::Exited;
        r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return r;
}

#endif

// ── Cache-bypass read semantics ─────────────────────────────────────────────────────────
bool direct_needs_alignment() {
#if defined(__APPLE__)
    return false; // F_NOCACHE is a caching hint, not an I/O mode: plain preads stay fully general
#else
    return true; // O_DIRECT / FILE_FLAG_NO_BUFFERING reject unaligned windows
#endif
}

// ── Reclaim-exempt allocation ────────────────────────────────────────────────────────────
// One section for every platform: Android and Darwin have such a store, everywhere else this
// reports "unsupported" and callers fall back to an ordinary allocation. Declared in the header with the measured
// properties and the reason the ceiling is a lock boundary rather than an allocation one.
#if !defined(_WIN32)
namespace {
// What this process may lock in place, as the kernel's own per-process limit states it.
uint64_t memlock_limit() {
    struct rlimit r {};
    if (getrlimit(RLIMIT_MEMLOCK, &r) != 0) return 0;
    return r.rlim_cur == RLIM_INFINITY ? lock_unbounded : (uint64_t) r.rlim_cur;
}
} // namespace
#endif

#if defined(__ANDROID__)

// A dma-buf is charged to no limit this process can read: gralloc publishes no total, so the
// answer is "unbounded here" and the caller bounds it by what the process may hold. Locking in
// place is a different store with a different answer - the vendor caps it at a few pages.
uint64_t lockable_bytes() {
    return lock_unbounded;
}
uint64_t lock_in_place_bytes() {
    return memlock_limit();
}

size_t pinned_max_bytes() {
    // The lock path uses a signed 32-bit type: AHardwareBuffer_lock returns EINVAL at exactly 2^31
    // bytes while 2^31 - 1 succeeds, even though allocation reaches the 32-bit width cap of 4 GiB.
    // Probing this at runtime would cost a multi-GiB allocation at startup to learn a constant, so
    // it is stated here and verified by `bmoe-membench --probe-max` instead.
    return 0x7FFFFFFFu;
}

bool pinned_alloc(size_t sz, PinnedAlloc * out) {
    if (!out || sz == 0 || sz > pinned_max_bytes()) return false;
    AHardwareBuffer_Desc d{};
    d.width = (uint32_t) sz; // BLOB: width IS the byte count, and height must be 1
    d.height = 1;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_BLOB;
    // CPU_READ_OFTEN asks gralloc for a cacheable mapping. Measured: the hint makes no difference
    // here (CPU_READ_RARELY reads identically), but asking for what we actually do is still right —
    // another gralloc may honour it.
    d.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

    AHardwareBuffer * buf = nullptr;
    if (AHardwareBuffer_allocate(&d, &buf) != 0 || !buf) return false;
    void * p = nullptr;
    if (AHardwareBuffer_lock(buf, d.usage, -1, nullptr, &p) != 0 || !p) {
        AHardwareBuffer_release(buf);
        return false;
    }
    out->base = p;
    out->handle = buf;
    out->size = sz;
    return true;
}

void pinned_free(PinnedAlloc * a) {
    if (!a || !a->handle) return;
    AHardwareBuffer * buf = (AHardwareBuffer *) a->handle;
    AHardwareBuffer_unlock(buf, nullptr);
    AHardwareBuffer_release(buf);
    a->base = nullptr;
    a->handle = nullptr;
    a->size = 0;
}

#elif defined(__APPLE__)

// Darwin's reclaim-exempt store is wired memory: an anonymous mapping the process has locked, which
// the kernel neither swaps nor compresses. Any process may ask, up to a system-wide limit that is
// most of RAM, so the limit is read rather than assumed.
size_t pinned_max_bytes() {
    uint64_t limit = 0;
    size_t len = sizeof(limit);
    if (sysctlbyname("vm.user_wire_limit", &limit, &len, nullptr, 0) != 0) return 0;
    return (size_t) limit;
}
// What is left of that limit, and the limit is GLOBAL: it bounds the wired memory of the whole
// system, the kernel's own included, not this process's share of it. Measured by locking anonymous
// memory in 32 MiB steps until the kernel refused: 10336 MiB granted where the limit less the
// system's wired count stood at 10368, on a machine whose limit is 12451. So a device that wires
// its buffers, or any other process that locks memory, shrinks this without this process having
// allocated anything - which is why it is read each time rather than derived from the limit.
uint64_t lockable_bytes() {
    uint64_t global = 0;
    size_t len = sizeof(global);
    if (sysctlbyname("vm.global_user_wire_limit", &global, &len, nullptr, 0) != 0) return 0;
    vm_statistics64_data_t vm;
    if (!host_vm(&vm)) return 0;
    const uint64_t wired = (uint64_t) vm.wire_count * (uint64_t) vm_page();
    const uint64_t left = global > wired ? global - wired : 0;
    return std::min({left, (uint64_t) pinned_max_bytes(), memlock_limit()});
}
// One store: a wired anonymous mapping is the same thing whether it was locked when it was
// allocated or afterwards.
uint64_t lock_in_place_bytes() {
    return lockable_bytes();
}
bool pinned_alloc(size_t sz, PinnedAlloc * out) {
    if (!sz || !out) return false;
    void * p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return false;
    if (mlock(p, sz) != 0) { // over the limit: say so by failing, as a full device would
        munmap(p, sz);
        return false;
    }
    out->base = p;
    out->handle = nullptr;
    out->size = sz;
    return true;
}
void pinned_free(PinnedAlloc * a) {
    if (!a || !a->base) return;
    munmap(a->base, a->size);
    *a = PinnedAlloc{};
}

#else

size_t pinned_max_bytes() {
    return 0;
}
// No reclaim-exempt allocation here, so nothing to total. Locking in place is whatever the
// process limit says, and on Windows nothing: VirtualLock is bounded by a working-set quota that
// is not ours to raise, which is why vm_pin declines there.
uint64_t lockable_bytes() {
    return 0;
}
uint64_t lock_in_place_bytes() {
#if defined(_WIN32)
    return 0;
#else
    return memlock_limit();
#endif
}
bool pinned_alloc(size_t, PinnedAlloc *) {
    return false;
}
void pinned_free(PinnedAlloc *) {}

#endif

} // namespace bmoe::pio
