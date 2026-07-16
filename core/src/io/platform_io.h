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

// How many of a committed range's pages the kernel still has in RAM. Counts only the pages fully
// inside [p, p+sz) — the same clipping vm_evict's callers apply — and ADDS to *sampled/*resident,
// so several ranges can be summed into one fraction (the caller zeroes them first).
//
// This is the reclaim sensor. Nothing unprivileged can ask Android how much memory it will concede
// (MemAvailable counts our own file-backed weights as free; PSI is SELinux-blocked for apps), but a
// process may always ask about its OWN pages — and pages we wrote, then lost, are reclaim by
// definition. One mincore() serves a whole chunk of pages, so the cost is per range, not per page.
//
// Returns false when the platform cannot report residency (Windows host build), leaving the
// counters untouched: "unmeasured", which callers must not read as "nothing is resident".
bool vm_resident_sample(const void * p, size_t sz, size_t * sampled, size_t * resident);

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

// Bytes one major fault moves: the page size. Faults are counted, but what a reader wants to know is
// how much memory the kernel took back and made us re-read — and a count only becomes that once it
// is multiplied by this. 194 MiB re-faulted in one token says what "47447 faults" does not.
size_t fault_bytes();

// Where this process's memory actually is, right now (/proc/self/status). The split is the whole
// point: the expert cache is ANONYMOUS, the model's weights are FILE-backed, and they are reclaimed
// by different mechanisms with different costs — anon goes to zram (swap), file pages are simply
// dropped. Watching them separately is how you tell "the kernel is taking my cache" apart from
// "the kernel is dropping the weights I mmap'd".
struct ProcessMemory {
    uint64_t rss_bytes = 0;      // VmRSS: everything resident
    uint64_t rss_anon_bytes = 0; // RssAnon: our cache lives here
    uint64_t rss_file_bytes = 0; // RssFile: the mmap'd model
    uint64_t swap_bytes = 0;     // VmSwap: anon we have already lost to zram
    uint64_t rss_peak_bytes = 0; // VmHWM
};
// False when the platform cannot report (Windows host), leaving `out` untouched.
bool process_memory(ProcessMemory * out);

// What the device says about itself (/proc/meminfo). MemAvailable is the number that lies (it counts
// our own mmap'd weights as reclaimable), which is exactly why it is worth recording next to what we
// measure ourselves: the gap between them IS the story.
struct DeviceMemory {
    uint64_t available_bytes = 0; // MemAvailable
    uint64_t free_bytes = 0;      // MemFree
    uint64_t swap_free_bytes = 0; // SwapFree
};
bool device_memory(DeviceMemory * out);

} // namespace bmoe::pio
