// A pooled, positioned file reader with optional cache-bypassing (O_DIRECT) I/O.
//
// One reader owns N lane fds and N bounce buffers, so N threads can read distinct byte ranges of the
// same file concurrently without contending. Each read aligns its window to the device block size,
// pulls it into the lane's bounce buffer, and memcpy's the requested interior out — the mechanics
// O_DIRECT requires. `direct` is a property of the reader, chosen by whoever opens it: the expert
// streamer and the dense-weights loader each construct their own, so one can bypass the page cache
// while the other does not — the two O_DIRECT decisions are independent, not a shared global.
//
// The O_DIRECT request is verified once at open: on storage where a direct read returns garbage
// (some FUSE-backed emulated volumes) the reader silently falls back to buffered I/O for the file,
// so a caller never has to reason about the storage — it asks for direct and gets correct bytes.
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
    // EOF tail an O_DIRECT read cannot cover) and a `bounce_cap`-byte aligned bounce per lane. `direct`
    // requests O_DIRECT; it is verified and silently downgraded on storage that mis-serves it. Reads
    // align to `align`. Returns false on any open/alloc failure. A reader is opened once and not reused.
    bool open(const std::string & path, int lanes, bool direct, size_t align, size_t bounce_cap);
    void close();

    bool is_open() const { return !fds_.empty(); }
    bool direct() const { return direct_; } // the effective mode after verification/fallback
    uint64_t file_size() const { return fsize_; }
    int lanes() const { return (int) fds_.size(); }

    // Read `nbytes` at file offset `off` into `dst`, on `lane` (0 <= lane < lanes()). Thread-safe
    // across distinct lanes — each has its own fd and bounce. Returns the aligned window actually
    // pulled from the drive (>= nbytes; what the bandwidth must be judged against), or -1 on I/O
    // error. A zero-length read is a no-op returning 0. The lane's bounce grows if a read needs more.
    long long read(int lane, void * dst, uint64_t off, uint64_t nbytes);

    // Aggregate accounting since open, summed across lanes.
    long long read_bytes() const { return read_bytes_.load(std::memory_order_relaxed); }
    long long syscall_ns() const { return syscall_ns_.load(std::memory_order_relaxed); }

private:
    std::vector<pio::fd_t> fds_;     // primary (maybe O_DIRECT) per lane
    std::vector<pio::fd_t> fds_buf_; // buffered fallback per lane, for the sub-alignment EOF tail
    std::vector<void *> bounces_;
    std::vector<size_t> bounce_sz_;
    size_t align_ = 4096;
    uint64_t fsize_ = 0;
    bool direct_ = false;
    std::atomic<long long> read_bytes_{0};
    std::atomic<long long> syscall_ns_{0};
};

} // namespace bmoe
