// A pooled, positioned file reader with optional cache-bypassing I/O (O_DIRECT on POSIX,
// FILE_FLAG_NO_BUFFERING on Windows, F_NOCACHE on Apple).
//
// One reader owns N lane fds and N bounce buffers, so N threads can read distinct byte ranges of the
// same file concurrently without contending. Where the platform's direct mode is an I/O mode that
// rejects unaligned access (O_DIRECT, NO_BUFFERING), a direct read aligns its window to the read
// alignment, pulls it into the lane's bounce buffer, and memcpy's the requested interior out — the
// mechanics those modes require. Apple's F_NOCACHE is not such a mode — a caching hint on the
// descriptor — so a direct reader there keeps ordinary pread semantics and skips the bounce
// entirely; see pio::direct_needs_alignment(). `direct` is a property of the reader, chosen by
// whoever opens it: the expert streamer and the dense-weights loader each construct their own, so
// one can bypass the page cache while the other does not — the two decisions are independent, not
// a shared global.
//
// The O_DIRECT request is verified once at open: on storage where a direct read returns garbage
// (some FUSE-backed emulated volumes) the reader silently falls back to buffered I/O for the file,
// so a caller never has to reason about the storage — it asks for direct and gets correct bytes.
// direct() reports the effective mode after all of that, which is what telemetry must print, not
// what the caller requested.
#pragma once

#include "platform_io.h"

#include <atomic>
#include <string>
#include <vector>

namespace bmoe {

class FileReader {
public:
    FileReader() = default;
    ~FileReader();

    FileReader(const FileReader &) = delete;
    FileReader & operator=(const FileReader &) = delete;

    // Open `path` with `lanes` independent primary fds (+ a buffered fd per lane for the sub-alignment
    // EOF tail an alignment-constrained direct read cannot cover) and a `bounce_cap`-byte aligned
    // bounce per lane. `direct` requests cache bypass; it is verified and silently downgraded where
    // the platform or the storage refuses or mis-serves it — direct() then reports the effective
    // mode. Reads align to `align` where the platform's direct mode demands it. Returns false on any
    // open/alloc failure. A reader is opened once and not reused.
    bool open(const std::string & path, int lanes, bool direct, size_t align, size_t bounce_cap);
    void close();

    bool is_open() const { return !fds_.empty(); }
    bool direct() const { return direct_; } // the effective cache-bypass mode, not the request
    uint64_t file_size() const { return fsize_; }
    int lanes() const { return (int) fds_.size(); }

    // Read `nbytes` at file offset `off` into `dst`, on `lane` (0 <= lane < lanes()). Thread-safe
    // across distinct lanes — each has its own fd and bounce. Returns what was actually pulled from
    // the drive — the aligned window (>= nbytes) where the platform's direct mode requires one,
    // exactly the requested bytes otherwise — which is what the bandwidth must be judged against;
    // or -1 on I/O error. A zero-length read is a no-op returning 0. The lane's bounce grows if a
    // windowed read needs more.
    long long read(int lane, void * dst, uint64_t off, uint64_t nbytes);

    // Aggregate accounting since open, summed across lanes.
    long long read_bytes() const { return read_bytes_.load(std::memory_order_relaxed); }
    long long syscall_ns() const { return syscall_ns_.load(std::memory_order_relaxed); }

private:
    std::vector<pio::fd_t> fds_;     // primary (cache-bypassing where achieved) per lane
    std::vector<pio::fd_t> fds_buf_; // buffered fallback per lane, for the sub-alignment EOF tail
    std::vector<void *> bounces_;
    std::vector<size_t> bounce_sz_;
    size_t align_ = 4096;
    uint64_t fsize_ = 0;
    bool direct_ = false;        // cache bypass actually in effect for the fds (uncached descriptors),
                                 // after the open's own report and every fallback
    bool aligned_reads_ = false; // direct_ AND the platform's direct mode rejects unaligned reads —
                                 // gates the read mechanics: window rounding, bounce, buffered tail fd
    std::atomic<long long> read_bytes_{0};
    std::atomic<long long> syscall_ns_{0};
};

} // namespace bmoe
