#include "file_reader.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace bmoe {

using clock_t_ = std::chrono::steady_clock;

FileReader::~FileReader() {
    close();
}

bool FileReader::open(const std::string & path, int lanes, bool direct, size_t align, size_t bounce_cap) {
    if (is_open()) return false;
    align_ = align ? align : 4096;
    direct_ = direct;
    const int n = lanes < 1 ? 1 : lanes;

    pio::fd_t primary = pio::open_read(path.c_str(), direct_);
    if (!pio::fd_ok(primary) && direct_) { // the platform refused O_DIRECT outright — try buffered
        direct_ = false;
        primary = pio::open_read(path.c_str(), false);
    }
    if (!pio::fd_ok(primary)) {
        std::fprintf(stderr, "bmoe: open %s failed\n", path.c_str());
        return false;
    }
    fsize_ = pio::file_size(primary);

    // Verify O_DIRECT actually returns correct bytes on this storage. On some emulated / FUSE-backed
    // volumes (e.g. an app-private dir under /storage/emulated) the open SUCCEEDS but direct reads
    // return garbage, silently corrupting weights → nonsense output. Compare one aligned block read
    // directly against the same block read buffered; on any mismatch, fall back to buffered I/O for
    // this file. On real filesystems (adb-pushed models, desktop) the two match and O_DIRECT is kept.
    if (direct_ && fsize_ >= (uint64_t) align_) {
        uint64_t voff = (fsize_ / 2) & ~(uint64_t) (align_ - 1);
        if (voff + align_ > fsize_) voff = 0;
        void * dbuf = pio::alloc_aligned(align_, align_);
        pio::fd_t vfd = pio::open_read(path.c_str(), false);
        if (dbuf && pio::fd_ok(vfd)) {
            std::vector<char> bbuf(align_);
            const long long want = (long long) align_;
            const long long gd = pio::pread_at(primary, dbuf, align_, voff);
            const long long gb = pio::pread_at(vfd, bbuf.data(), align_, voff);
            const bool bad = gd != want || gb != want || std::memcmp(dbuf, bbuf.data(), align_) != 0;
            if (bad) {
                std::fprintf(stderr, "bmoe: O_DIRECT returns wrong data on this storage — using buffered I/O\n");
                direct_ = false;
                pio::close_fd(primary);
                primary = pio::open_read(path.c_str(), false);
            }
        }
        if (dbuf) pio::aligned_free(dbuf);
        if (pio::fd_ok(vfd)) pio::close_fd(vfd);
        if (!pio::fd_ok(primary)) {
            std::fprintf(stderr, "bmoe: reopen after O_DIRECT check failed\n");
            return false;
        }
    }

    // A private fd + bounce per lane so concurrent reads never contend. Lane 0 inherits the fd already
    // opened (and verified) above; the rest open their own.
    fds_.assign(n, pio::fd_invalid);
    fds_buf_.assign(n, pio::fd_invalid);
    bounces_.assign(n, nullptr);
    bounce_sz_.assign(n, 0);
    for (int lane = 0; lane < n; ++lane) {
        fds_[lane] = (lane == 0) ? primary : pio::open_read(path.c_str(), direct_);
        fds_buf_[lane] = direct_ ? pio::open_read(path.c_str(), false) : pio::fd_invalid;
        if (!pio::fd_ok(fds_[lane])) {
            std::fprintf(stderr, "bmoe: lane %d open failed\n", lane);
            close();
            return false;
        }
        bounces_[lane] = pio::alloc_aligned(align_, bounce_cap);
        if (!bounces_[lane]) {
            std::fprintf(stderr, "bmoe: lane %d bounce alloc failed\n", lane);
            close();
            return false;
        }
        bounce_sz_[lane] = bounce_cap;
    }
    return true;
}

long long FileReader::read(int lane, void * dst, uint64_t off, uint64_t nbytes) {
    if (nbytes == 0) return 0;
    char * interior = nullptr;
    const long long window = fill_window(lane, off, nbytes, &interior);
    if (window < 0) return -1;
    std::memcpy(dst, interior, (size_t) nbytes);
    return window;
}

long long FileReader::read_scatter(int lane, uint64_t off, const Span * spans, int n_spans) {
    uint64_t total = 0;
    for (int i = 0; i < n_spans; ++i)
        total += spans[i].nbytes;
    if (total == 0) return 0;
    char * interior = nullptr;
    const long long window = fill_window(lane, off, total, &interior);
    if (window < 0) return -1;
    for (int i = 0; i < n_spans; ++i) {
        if (spans[i].nbytes == 0) continue;
        std::memcpy(spans[i].dst, interior, (size_t) spans[i].nbytes);
        interior += spans[i].nbytes;
    }
    return window;
}

long long FileReader::fill_window(int lane, uint64_t off, uint64_t nbytes, char ** interior) {
    const uint64_t a0 = off & ~(uint64_t) (align_ - 1);
    const uint64_t a1 = (off + nbytes + align_ - 1) & ~(uint64_t) (align_ - 1);
    const size_t len = (size_t) (a1 - a0);
    if (bounce_sz_[lane] < len) {
        if (bounces_[lane]) pio::aligned_free(bounces_[lane]);
        bounces_[lane] = pio::alloc_aligned(align_, len);
        if (!bounces_[lane]) {
            std::fprintf(stderr, "bmoe: bounce realloc %zu failed\n", len);
            return -1;
        }
        bounce_sz_[lane] = len;
    }
    char * b = (char *) bounces_[lane];
    const pio::fd_t fd = fds_[lane];
    const pio::fd_t fd_buf = fds_buf_[lane];
    const uint64_t read_end = (fsize_ && a1 > fsize_) ? fsize_ : a1;
    const uint64_t bulk_end = direct_ ? (read_end & ~(uint64_t) (align_ - 1)) : read_end;

    const auto t0 = clock_t_::now();
    for (uint64_t a = a0; a < bulk_end;) {
        long long got = pio::pread_at(fd, b + (a - a0), (size_t) (bulk_end - a), a);
        if (got <= 0) {
            std::fprintf(stderr, "bmoe: pread failed at %llu\n", (unsigned long long) a);
            return -1;
        }
        a += (uint64_t) got;
    }
    for (uint64_t a = bulk_end; a < read_end;) { // sub-alignment EOF tail via the buffered fd
        long long got = pio::pread_at(pio::fd_ok(fd_buf) ? fd_buf : fd, b + (a - a0), (size_t) (read_end - a), a);
        if (got <= 0) {
            std::fprintf(stderr, "bmoe: tail pread failed at %llu\n", (unsigned long long) a);
            return -1;
        }
        a += (uint64_t) got;
    }
    const auto t1 = clock_t_::now();
    syscall_ns_.fetch_add((long long) std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    *interior = b + (off - a0);
    const long long window = (long long) (read_end - a0);
    read_bytes_.fetch_add(window);
    return window; // the aligned window pulled — what the effective bandwidth is judged against
}

void FileReader::close() {
    for (pio::fd_t fd : fds_)
        if (pio::fd_ok(fd)) pio::close_fd(fd);
    for (pio::fd_t fd : fds_buf_)
        if (pio::fd_ok(fd)) pio::close_fd(fd);
    for (void * b : bounces_)
        if (b) pio::aligned_free(b);
    fds_.clear();
    fds_buf_.clear();
    bounces_.clear();
    bounce_sz_.clear();
}

} // namespace bmoe
