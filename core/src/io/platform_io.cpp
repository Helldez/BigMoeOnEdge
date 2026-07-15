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

size_t vm_lock(void * p, size_t sz) {
    if (!p || !sz) return 0;
    // VirtualLock draws from the process working set, which carries a small default quota — grow it
    // first or the lock fails as soon as that quota is reached. Ask for the range plus slack for
    // what the process already holds; if the grow is refused the lock loop still reports the truth.
    SIZE_T mn = 0, mx = 0;
    if (GetProcessWorkingSetSize(GetCurrentProcess(), &mn, &mx)) {
        const SIZE_T want = (SIZE_T) sz + (64u << 20);
        SetProcessWorkingSetSize(GetCurrentProcess(), mn + want, mx + want);
    }
    const size_t chunk = 64ull << 20; // truncate at the quota rather than fail the whole range
    size_t done = 0;
    while (done < sz) {
        const size_t n = sz - done < chunk ? sz - done : chunk;
        if (!VirtualLock((char *) p + done, n)) break;
        done += n;
    }
    return done;
}

void vm_unlock(void * p, size_t sz) {
    if (p && sz) VirtualUnlock(p, sz);
}

uint64_t vm_lock_limit_raise() {
    return ~0ull; // no RLIMIT_MEMLOCK equivalent; the working-set quota is handled inside vm_lock
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

size_t vm_lock(void * p, size_t sz) {
#if defined(_POSIX_MEMLOCK_RANGE)
    if (!p || !sz) return 0;
    // Chunked: mlock() is all-or-nothing per call, so one call over the whole dense set would fail
    // outright against a small RLIMIT_MEMLOCK and pin nothing. Locking in chunks pins what fits and
    // stops at the first refusal — the remainder degrades to ordinary reclaimable page cache.
    const size_t chunk = 64ull << 20;
    size_t done = 0;
    while (done < sz) {
        const size_t n = sz - done < chunk ? sz - done : chunk;
        if (mlock((char *) p + done, n) != 0) break;
        done += n;
    }
    return done;
#else
    (void) p;
    (void) sz;
    return 0;
#endif
}

void vm_unlock(void * p, size_t sz) {
#if defined(_POSIX_MEMLOCK_RANGE)
    if (p && sz) munlock(p, sz);
#else
    (void) p;
    (void) sz;
#endif
}

uint64_t vm_lock_limit_raise() {
#if defined(RLIMIT_MEMLOCK)
    struct rlimit rl;
    if (getrlimit(RLIMIT_MEMLOCK, &rl) != 0) return 0;
    if (rl.rlim_cur != rl.rlim_max) {
        // Raising the soft limit up to the hard one needs no privilege. The hard limit is set by the
        // OS/container (on Android it is typically generous for a shell process and tight for an
        // app), and lifting THAT would need CAP_SYS_RESOURCE — out of scope for a library.
        struct rlimit want = rl;
        want.rlim_cur = rl.rlim_max;
        if (setrlimit(RLIMIT_MEMLOCK, &want) == 0) rl = want;
    }
    return rl.rlim_cur == RLIM_INFINITY ? ~0ull : (uint64_t) rl.rlim_cur;
#else
    return ~0ull;
#endif
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

#endif

} // namespace bmoe::pio
