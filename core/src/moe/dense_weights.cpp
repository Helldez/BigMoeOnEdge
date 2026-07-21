#include "dense_weights.h"

#include "ggml.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace bmoe {

using clock_t_ = std::chrono::steady_clock;

DenseWeights::~DenseWeights() {
    shutdown();
}

std::vector<std::pair<uint64_t, uint64_t>>
DenseWeights::byte_ranges(std::vector<std::pair<uint64_t, uint64_t>> expert_ranges, uint64_t file_size) {
    std::sort(expert_ranges.begin(), expert_ranges.end());
    std::vector<std::pair<uint64_t, uint64_t>> dense;
    uint64_t pos = 0;
    for (const auto & r : expert_ranges) {
        if (r.first > pos) dense.push_back({pos, r.first}); // the gap before this expert range is dense
        pos = std::max(pos, r.second);
    }
    if (pos < file_size) dense.push_back({pos, file_size}); // the trailing dense tail (lm_head et al.)
    return dense;
}

bool DenseWeights::init(DenseWeightsMode mode,
                        const std::string & path,
                        size_t align,
                        std::vector<std::pair<uint64_t, uint64_t>> ranges,
                        std::vector<DenseTensorRef> tensors) {
    mode_ = mode;
    path_ = path;
    align_ = align ? align : 4096;
    ranges_ = std::move(ranges);
    tensors_ = std::move(tensors);
    const size_t slash = path_.find_last_of("/\\");
    basename_ = slash == std::string::npos ? path_ : path_.substr(slash + 1);

    if (mode_ == DenseWeightsMode::Anonymous || mode_ == DenseWeightsMode::Pinned) {
        if (tensors_.empty()) return true; // nothing captured to rebind — behave as Mmap
        if (mode_ == DenseWeightsMode::Pinned && pio::pinned_max_bytes() == 0) {
            std::fprintf(stderr, "bmoe: --dense-weights ahwb needs reclaim-exempt memory, which this "
                                 "platform does not provide (Android only)\n");
            return false;
        }
        // A single-lane reader with a bounce large enough for our chunk; O_DIRECT independent of the
        // expert stream. Sized to the largest tensor is unnecessary — we read in bounded chunks.
        const size_t chunk = 8ull << 20;
        if (!reader_.open(path_, 1, /*direct=*/true, align_, chunk + 2 * align_)) return false;
        if (!read_anonymous(align_)) return false;
        drop_mmap_copies(pio::vm_page());
    } else if (mode_ == DenseWeightsMode::Warmed) {
        warm();
    }
    return true;
}

// ── Anonymous / Pinned: read each dense tensor whole into our own buffer and rebind onto it ──
//
// The two modes differ ONLY in where the buffer comes from: ordinary anonymous memory, which the
// kernel may reclaim to zram, or a dma-buf it may not touch at all. Everything else — the O_DIRECT
// read, the rebind, handing back the now-unreferenced mmap pages — is identical, which is what makes
// the A/B between them a single-variable experiment rather than two code paths being compared.
//
// Pinned allocates PER TENSOR, so the 2047 MiB ceiling on a single dma-buf is not a constraint in
// practice: the largest dense tensor here is an embedding or lm_head, far below it. A tensor that
// did exceed it fails the run rather than quietly taking an anon buffer, because a silent mix would
// make the comparison meaningless in exactly the direction that flatters the feature.
bool DenseWeights::read_anonymous(size_t align) {
    const bool pinned = mode_ == DenseWeightsMode::Pinned;
    const uint64_t chunk = 8ull << 20;
    uint64_t total = 0;
    bufs_.reserve(tensors_.size());
    buf_sz_.reserve(tensors_.size());
    if (pinned) pinned_.reserve(tensors_.size());
    for (const DenseTensorRef & d : tensors_) {
        if (!d.tensor || d.size == 0) continue;
        void * buf = nullptr;
        if (pinned) {
            pio::PinnedAlloc pa;
            if (!pio::pinned_alloc((size_t) d.size, &pa)) {
                std::fprintf(stderr, "bmoe: pinned dense buffer %llu MiB failed (ceiling %llu MiB)\n",
                             (unsigned long long) (d.size >> 20), (unsigned long long) (pio::pinned_max_bytes() >> 20));
                return false;
            }
            pinned_.push_back(pa); // tracked for shutdown even if a chunk read below fails
            buf = pa.base;
        } else {
            buf = pio::alloc_aligned(align, (size_t) d.size);
            if (!buf) {
                std::fprintf(stderr, "bmoe: dense buffer alloc %llu failed\n", (unsigned long long) d.size);
                return false;
            }
            bufs_.push_back(buf); // tracked for shutdown even if a chunk read below fails
        }
        bases_.push_back(buf); // what the sensor probes, whichever allocator produced it
        buf_sz_.push_back((size_t) d.size);
        for (uint64_t done = 0; done < d.size;) {
            const uint64_t n = std::min<uint64_t>(chunk, d.size - done);
            if (reader_.read(0, (char *) buf + done, d.file_off + done, n) < 0) return false;
            done += n;
        }
        d.tensor->data = buf; // rebind the model weight onto its private copy
        total += d.size;
    }
    std::fprintf(stderr, "bmoe: dense-weights=%s — %llu MiB in %zu %s buffers\n", pinned ? "ahwb" : "anon",
                 (unsigned long long) (total >> 20), buf_sz_.size(), pinned ? "pinned" : "anon");
    return true;
}

// Hand the mmap copies back. The capture warm-up decode faulted these dense pages in mmap-resident,
// and read_anonymous has just copied them into anon buffers and rebound every tensor — so the file-
// backed pages are referenced by nobody. Left alone they sit resident until reclaim, doubling the
// dense footprint at the worst moment (right before prefill). Drop them with MADV_DONTNEED: a clean
// read-only mapping, so nothing is lost and nothing will refault the range. Best-effort — needs
// /proc/self/maps to turn a file offset into an address; where that is unreadable the pages stay.
void DenseWeights::drop_mmap_copies(size_t page) {
    std::vector<pio::MappedRegion> vmas;
    if (!pio::file_mapped_regions(basename_.c_str(), vmas) || vmas.empty()) return;
    auto addr_of = [&](uint64_t off) -> char * {
        for (const auto & v : vmas) {
            const uint64_t span = (uint64_t) (v.end - v.start);
            if (off >= v.file_offset && off < v.file_offset + span) return (char *) v.start + (off - v.file_offset);
        }
        return nullptr;
    };
    uint64_t dropped = 0;
    for (const DenseTensorRef & d : tensors_) {
        if (!d.tensor || d.size == 0) continue;
        char * a = addr_of(d.file_off);
        if (!a) continue;
        // Align INWARD to whole pages (start up, end down), so a page shared with an adjacent tensor
        // that stays mmap-resident — an expert slice, or the next dense tensor — is never dropped.
        uintptr_t a0 = ((uintptr_t) a + page - 1) & ~(uintptr_t) (page - 1);
        uintptr_t a1 = ((uintptr_t) a + d.size) & ~(uintptr_t) (page - 1);
        if (a1 > a0) {
            pio::vm_drop_file_pages((void *) a0, (size_t) (a1 - a0));
            dropped += a1 - a0;
        }
    }
    if (dropped)
        std::fprintf(stderr, "bmoe: dense-weights=%s — dropped %llu MiB of now-unused mmap pages\n",
                     mode_ == DenseWeightsMode::Pinned ? "ahwb" : "anon", (unsigned long long) (dropped >> 20));
}

// ── Warmed: one sequential buffered sweep over the dense ranges to populate the page cache ──
void DenseWeights::warm() {
    pio::fd_t fd = pio::open_read(path_.c_str(), false);
    if (!pio::fd_ok(fd)) return;
    const size_t chunk = 8ull << 20;
    void * buf = pio::alloc_aligned(align_, chunk);
    if (!buf) {
        pio::close_fd(fd);
        return;
    }
    const auto t0 = clock_t_::now();
    uint64_t warmed = 0;
    bool ok = true;
    for (const auto & r : ranges_) {
        for (uint64_t a = r.first; a < r.second && ok;) {
            const long long got = pio::pread_at(fd, buf, (size_t) std::min<uint64_t>(chunk, r.second - a), a);
            if (got <= 0) {
                ok = false; // best-effort: those pages just stay cold
                break;
            }
            a += (uint64_t) got;
            warmed += (uint64_t) got;
        }
    }
    pio::aligned_free(buf);
    pio::close_fd(fd);
    const double s = std::chrono::duration<double>(clock_t_::now() - t0).count();
    std::fprintf(stderr, "bmoe: dense warm-up — %llu MiB in %.1f s%s\n", (unsigned long long) (warmed >> 20), s,
                 ok ? "" : " (partial)");
}

// ── residency sensor ─────────────────────────────────────────────────────────────────
void DenseWeights::sample_residency(size_t page) {
    if (mode_ == DenseWeightsMode::Anonymous || mode_ == DenseWeightsMode::Pinned)
        sample_anon(page);
    else
        sample_mmap(page);
}

// Anonymous/Pinned: mincore our own buffers directly. Anon memory is reclaimed to zram, and mincore
// reports resident anon pages just as it does file pages, so resident_frac keeps its meaning under
// the flag. Under Pinned the fraction is the EXPERIMENT rather than a diagnostic: dma-buf pages are
// supposed to be reclaim-exempt, so anything below 1.0 falsifies the premise. Should mincore not
// report on a dma-buf mapping at all, vm_resident_sample leaves the counters alone and the fraction
// stays -1 — unmeasured, which must not be read as "nothing is resident".
void DenseWeights::sample_anon(size_t page) {
    if (bases_.empty()) return;
    uint64_t total = 0;
    for (size_t sz : buf_sz_)
        total += sz;
    if (total == 0) return;
    size_t sampled = 0, resident = 0;
    for (int k = 0; k < sample_pages; ++k) {
        uint64_t target = (total * (uint64_t) k) / (uint64_t) sample_pages;
        for (size_t i = 0; i < bases_.size(); ++i) {
            if (target < buf_sz_[i]) {
                const char * a = (const char *) bases_[i] + target;
                const char * pg = (const char *) ((uintptr_t) a & ~(uintptr_t) (page - 1));
                pio::vm_resident_sample(pg, page, &sampled, &resident);
                break;
            }
            target -= buf_sz_[i];
        }
    }
    resident_frac_ = sampled ? (double) resident / (double) sampled : -1.0;
}

// Mmap/Warmed: the weights are one mmap of the gguf; a dense file offset becomes an address through
// the VMA that backs it (/proc/self/maps), which an app may read, being its own. Probe sample_pages
// points spread evenly across the dense byte ranges, one page each.
void DenseWeights::sample_mmap(size_t page) {
    if (ranges_.empty()) return;
    if (!vmas_tried_) { // resolve the mmap once; llama.cpp has mapped the file by the first decode
        vmas_tried_ = true;
        pio::file_mapped_regions(basename_.c_str(), vmas_);
    }
    if (vmas_.empty()) return; // maps unreadable → leave resident_frac_ at -1

    uint64_t total = 0;
    for (const auto & r : ranges_)
        total += r.second - r.first;
    if (total == 0) return;

    auto addr_of = [&](uint64_t off) -> const char * {
        for (const auto & v : vmas_) {
            const uint64_t span = (uint64_t) (v.end - v.start);
            if (off >= v.file_offset && off < v.file_offset + span)
                return (const char *) v.start + (off - v.file_offset);
        }
        return nullptr;
    };

    size_t sampled = 0, resident = 0;
    for (int k = 0; k < sample_pages; ++k) {
        uint64_t target = (total * (uint64_t) k) / (uint64_t) sample_pages;
        uint64_t off = 0;
        for (const auto & r : ranges_) {
            const uint64_t len = r.second - r.first;
            if (target < len) {
                off = r.first + target;
                break;
            }
            target -= len;
        }
        // Align the probe DOWN to its page: vm_resident_sample counts only pages fully inside the
        // range, so a single page handed in at an arbitrary offset would clip to nothing.
        if (const char * a = addr_of(off)) {
            const char * pg = (const char *) ((uintptr_t) a & ~(uintptr_t) (page - 1));
            pio::vm_resident_sample(pg, page, &sampled, &resident);
        }
    }
    resident_frac_ = sampled ? (double) resident / (double) sampled : -1.0;
}

void DenseWeights::shutdown() {
    for (void * b : bufs_)
        if (b) pio::aligned_free(b);
    for (pio::PinnedAlloc & p : pinned_)
        pio::pinned_free(&p);
    bufs_.clear();
    pinned_.clear();
    bases_.clear();
    buf_sz_.clear();
    tensors_.clear();
    reader_.close();
}

} // namespace bmoe
