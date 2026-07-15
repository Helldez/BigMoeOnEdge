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

// Pin an already-mapped byte range into RAM so the kernel cannot reclaim it (mlock / VirtualLock),
// and release the pin. Used to hold the model's dense weights resident: warming them only fills the
// (reclaimable) page cache, which expert-streaming pressure then evicts, re-faulting them from
// flash inside the next decode.
//
// Best-effort by contract, because the amount of lockable memory is capped by the OS (POSIX
// RLIMIT_MEMLOCK — often small on Android) and the cap is a policy we must live with, not fail on:
// vm_lock locks in chunks and returns the bytes it actually pinned, which may be less than `sz`
// (0 = nothing). What is not pinned simply stays ordinary reclaimable page cache, i.e. today's
// behaviour. `p` must be page-aligned; `sz` is rounded up to a page by the OS.
size_t vm_lock(void * p, size_t sz);
void vm_unlock(void * p, size_t sz);

// Raise the process's lockable-memory soft limit to its hard limit and report the resulting cap in
// bytes (UINT64_MAX = unlimited, 0 = unknown). Raising the SOFT limit needs no privilege; the hard
// limit is what the OS/container grants, so this is the whole of what a process can do for itself.
// Call once before a batch of vm_lock calls.
uint64_t vm_lock_limit_raise();

// Physical memory currently allocatable without paging, in bytes. 0 = unknown. Used to size the
// expert cache to the device (--cache-mb auto) and to shrink it under memory pressure at runtime.
uint64_t mem_available_bytes();

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
