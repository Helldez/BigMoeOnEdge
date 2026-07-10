#include "platform_io.h"

namespace bmoe::pio {

#if defined(_WIN32)

#include <windows.h>
#include <malloc.h>

const fd_t fd_invalid = (void *) INVALID_HANDLE_VALUE;
bool fd_ok(fd_t fd) { return fd != (void *) INVALID_HANDLE_VALUE; }

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
    ov.Offset     = (DWORD) (off & 0xFFFFFFFFull);
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

void * aligned_alloc(size_t align, size_t sz) { return _aligned_malloc(sz, align); }
void   aligned_free(void * p) { if (p) _aligned_free(p); }

size_t vm_page() { SYSTEM_INFO si; GetSystemInfo(&si); return (size_t) si.dwPageSize; }
void * vm_reserve(size_t sz) { return VirtualAlloc(nullptr, sz, MEM_RESERVE, PAGE_READWRITE); }
bool   vm_commit(void * p, size_t sz) { return VirtualAlloc(p, sz, MEM_COMMIT, PAGE_READWRITE) != nullptr; }
void   vm_evict(void * p, size_t sz) { if (sz) VirtualFree(p, sz, MEM_DECOMMIT); }
void   vm_release(void * p, size_t /*sz*/) { if (p) VirtualFree(p, 0, MEM_RELEASE); }

#else

#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/mman.h>

#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

const fd_t fd_invalid = -1;
bool fd_ok(fd_t fd) { return fd >= 0; }

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

void * aligned_alloc(size_t align, size_t sz) {
    void * p = nullptr;
    return posix_memalign(&p, align, sz) == 0 ? p : nullptr;
}
void aligned_free(void * p) { free(p); }

size_t vm_page() { long ps = sysconf(_SC_PAGESIZE); return ps > 0 ? (size_t) ps : 4096; }
void * vm_reserve(size_t sz) {
    void * p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
bool   vm_commit(void * /*p*/, size_t /*sz*/) { return true; }  // POSIX commits on first touch
void   vm_evict(void * p, size_t sz) { if (sz) madvise(p, sz, MADV_DONTNEED); }
void   vm_release(void * p, size_t sz) { if (p) munmap(p, sz); }

#endif

} // namespace bmoe::pio
