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
using fd_t = void *; // HANDLE
#else
using fd_t = int;
#endif

extern const fd_t fd_invalid;
bool fd_ok(fd_t fd);

// Open path for positioned reads. When direct is true, request cache-bypassing I/O;
// the caller should be prepared to reopen with direct=false for a sub-alignment tail.
fd_t open_read(const char * path, bool direct);
void close_fd(fd_t fd);
// Positioned blocking read. Returns bytes read, 0 at EOF, -1 on error.
long long pread_at(fd_t fd, void * buf, size_t count, uint64_t off);
uint64_t file_size(fd_t fd);

// Aligned heap allocation for O_DIRECT bounce buffers and shared slots.
void * alloc_aligned(size_t align, size_t sz);
void aligned_free(void * p);

// Reserved (address-only) region; physical pages appear on commit, vanish on evict.
size_t vm_page();
void * vm_reserve(size_t sz);
bool vm_commit(void * p, size_t sz);
void vm_evict(void * p, size_t sz);
void vm_release(void * p, size_t sz);

// Ask the kernel to bring a range back into RAM now, sequentially (MADV_WILLNEED). Advisory and
// asynchronous: it queues the pages, it does not pin them and does not guarantee they arrive.
// Restoring a swapped-out range this way runs at the swap device's sequential rate, versus the
// scattered per-page major faults the same range would otherwise take on first touch.
// A no-op where the platform has no equivalent.
void vm_willneed(void * p, size_t sz);

// Physical memory currently allocatable without paging, in bytes. 0 = unknown. Used to size the
// expert cache to the device (--cache-mb auto) and to shrink it under memory pressure at runtime.
uint64_t mem_available_bytes();

// Bytes of THIS process's anonymous memory the kernel has pushed to swap, or 0 when the platform
// cannot report it. On Android that swap is zram: the reclaim daemon compresses the pages of an
// idle process, so an engine that sat between two prompts finds its expert cache and KV swapped
// out and pays to decompress them one major fault at a time. This is the signal that says how
// much was taken, and thus whether a bulk rewarm is worth doing before decoding. See
// docs/rewarm.md.
uint64_t process_swapped_bytes();

// Process-wide compute-decomposition counters, cumulative since process start; the caller deltas
// them across a single decode to split the per-token "compute" residual into its real causes.
// Both return 0 when the platform cannot report them (Windows host build), which the metrics treat
// as "unmeasured" rather than "zero work".
//
//   * major_faults(): hard page faults served from backing store (getrusage ru_majflt). A non-zero
//     per-token delta means a mmap-resident weight was re-faulted from flash *inside* the decode —
//     the >RAM residency stall that would otherwise masquerade as compute.
//   * process_cpu_seconds(): CPU time summed across all threads (CLOCK_PROCESS_CPUTIME_ID). Compared
//     against wall×threads it reveals occupancy: cpu≈wall×threads is genuine compute-bound work;
//     cpu≪wall×threads means the threads were descheduled or blocked (frequency cap, preemption,
//     fault wait) rather than computing.
uint64_t major_faults();
double process_cpu_seconds();

} // namespace bmoe::pio
