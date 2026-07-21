#include "platform_io.h"

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
#include <sys/mman.h>
#include <sys/resource.h>
#include <ctime>
#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#if defined(__ANDROID__)
#include <android/hardware_buffer.h> // reclaim-exempt allocation; see pinned_alloc
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

fd_t open_read(const char * path, bool direct) {
    DWORD flags = FILE_ATTRIBUTE_NORMAL | (direct ? FILE_FLAG_NO_BUFFERING : 0);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
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

fd_t open_read(const char * path, bool direct) {
    return open(path, O_RDONLY | O_CLOEXEC | (direct ? O_DIRECT : 0));
}

void close_fd(fd_t fd) {
    if (fd_ok(fd)) close(fd);
}

long long pread_at(fd_t fd, void * buf, size_t count, uint64_t off) {
    return (long long) pread(fd, buf, count, (off_t) off);
}

uint64_t file_size(fd_t fd) {
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    return sz > 0 ? (uint64_t) sz : 0;
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
    void * p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
bool vm_commit(void * /*p*/, size_t /*sz*/) {
    return true; // POSIX commits on first touch
}
void vm_evict(void * p, size_t sz) {
    if (sz) madvise(p, sz, MADV_DONTNEED);
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

uint64_t mem_available_bytes() {
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
    // Fallback where /proc is absent (e.g. macOS): free physical pages. An underestimate — it omits
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

// ── Reclaim-exempt allocation ────────────────────────────────────────────────────────────
// Shared across platforms because only Android has one: everywhere else this reports "unsupported"
// and callers fall back to an ordinary allocation. Declared in the header with the measured
// properties and the reason the ceiling is a lock boundary rather than an allocation one.
#if defined(__ANDROID__)

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

#else

size_t pinned_max_bytes() {
    return 0;
}
bool pinned_alloc(size_t, PinnedAlloc *) {
    return false;
}
void pinned_free(PinnedAlloc *) {}

#endif

} // namespace bmoe::pio
