// Cross-platform positioned I/O and reserved virtual memory.
//
// Two primitive families the expert streamer is built on:
//
//   * Positioned reads with optional cache bypass (O_DIRECT on POSIX,
//     FILE_FLAG_NO_BUFFERING on Windows). Reads are done on aligned windows into an
//     aligned bounce buffer; the caller memcpy's the valid interior out.
//
//   * Reserve/commit/evict/release of virtual address ranges. The LRU expert cache
//     reserves a full-size address range per (layer, projection) but commits physical
//     pages only for the expert slices actually cached, and hands them back on
//     eviction — so a huge reserved span costs RAM only for resident experts.
//
// POSIX commits lazily (commit is a no-op, MADV_DONTNEED reclaims); Windows commits and
// decommits explicitly. Both honour a page-granularity that the caller rounds to.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bmoe::pio {

#if defined(_WIN32)
using fd_t = void *;                      // HANDLE
#else
using fd_t = int;
#endif

extern const fd_t fd_invalid;
bool fd_ok(fd_t fd);

// Open path for positioned reads. When direct is true, request cache-bypassing I/O;
// the caller should be prepared to reopen with direct=false for a sub-alignment tail.
fd_t              open_read(const char * path, bool direct);
void              close_fd(fd_t fd);
// Positioned blocking read. Returns bytes read, 0 at EOF, -1 on error.
long long         pread_at(fd_t fd, void * buf, size_t count, uint64_t off);
uint64_t          file_size(fd_t fd);

// Aligned heap allocation for O_DIRECT bounce buffers and shared slots.
void * aligned_alloc(size_t align, size_t sz);
void   aligned_free(void * p);

// Reserved (address-only) region; physical pages appear on commit, vanish on evict.
size_t vm_page();
void * vm_reserve(size_t sz);
bool   vm_commit(void * p, size_t sz);
void   vm_evict(void * p, size_t sz);
void   vm_release(void * p, size_t sz);

} // namespace bmoe::pio
