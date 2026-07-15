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
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/resource.h>
#include <ctime>
#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#endif

namespace bmoe::pio {

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
void vm_willneed(void * /*p*/, size_t /*sz*/) {
    // PrefetchVirtualMemory is the nearest equivalent, but nothing on this build path needs it:
    // rewarm is driven by process_swapped_bytes(), which reports 0 here, so it never runs.
}

uint64_t mem_available_bytes() {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    return GlobalMemoryStatusEx(&ms) ? (uint64_t) ms.ullAvailPhys : 0;
}

// Windows pages to a file rather than to a compressed in-RAM device, and the host build exists for
// the byte-identity gates, not for perf on a memory-constrained phone. 0 = "nothing was taken",
// which keeps rewarm off on this platform.
uint64_t process_swapped_bytes() {
    return 0;
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
void vm_willneed(void * p, size_t sz) {
    // Best-effort by contract: the kernel may ignore the hint under pressure, and a failure only
    // leaves the pages where they were — the caller's next touch faults them in as it would have
    // anyway. Nothing here can make the range less resident than it already is.
    if (sz) madvise(p, sz, MADV_WILLNEED);
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

uint64_t process_swapped_bytes() {
    // VmSwap is this process's anonymous memory currently held in swap (zram on Android). It is the
    // only field that distinguishes "the kernel took my pages" from "I never allocated them": RSS
    // falling could equally mean the cache was evicted on purpose, whereas VmSwap rising means the
    // pages are still ours, just compressed and waiting to be faulted back one by one.
    if (FILE * f = std::fopen("/proc/self/status", "re")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long long kb = 0;
            if (std::sscanf(line, "VmSwap: %llu kB", &kb) == 1) {
                std::fclose(f);
                return (uint64_t) kb * 1024ull;
            }
        }
        std::fclose(f);
    }
    return 0; // no /proc (macOS) — treat as "unmeasured", which keeps rewarm off
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

#endif

} // namespace bmoe::pio
