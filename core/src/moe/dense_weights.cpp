#include "dense_weights.h"

#include "ggml.h"
#include "row_stream.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace bmoe {

using clock_t_ = std::chrono::steady_clock;

DenseWeights::DenseWeights() = default;

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
                        const std::vector<std::string> & paths,
                        size_t align,
                        std::vector<std::vector<std::pair<uint64_t, uint64_t>>> ranges,
                        std::vector<DenseTensorRef> tensors) {
    mode_ = mode;
    paths_ = paths;
    align_ = align ? align : 4096;
    ranges_ = std::move(ranges);
    tensors_ = std::move(tensors);
    basenames_.clear();
    for (const std::string & p : paths_) {
        const size_t slash = p.find_last_of("/\\");
        basenames_.push_back(slash == std::string::npos ? p : p.substr(slash + 1));
    }

    hold_back_oversized();
    take_row_gathered();

    if (mode_ == DenseWeightsMode::Anonymous || mode_ == DenseWeightsMode::Pinned) {
        if (tensors_.empty()) { // nothing (left) to rebind — behave as Mmap
            advise_random_mapped();
            return true;
        }
        if (mode_ == DenseWeightsMode::Pinned && pio::pinned_max_bytes() == 0) {
            std::fprintf(stderr, "bmoe: --dense-weights ahwb needs reclaim-exempt memory, which this "
                                 "platform does not provide (Android only)\n");
            return false;
        }
        // Single-lane readers (one per shard) with a bounce large enough for our chunk; O_DIRECT
        // independent of the expert stream. Sized to the largest tensor is unnecessary — we read in
        // bounded chunks.
        const size_t chunk = 8ull << 20;
        for (const std::string & p : paths_) {
            readers_.push_back(std::unique_ptr<FileReader>(new FileReader()));
            if (!readers_.back()->open(p, 1, /*direct=*/true, align_, chunk + 2 * align_)) return false;
        }
        if (!read_anonymous(align_)) return false;
        // The tensors are copied and rebound; nothing reads through these again. Their fds and
        // per-lane bounce buffers would otherwise sit allocated for the whole session, next to
        // the expert cache that is counting every MiB.
        readers_.clear();
        drop_mmap_copies(pio::vm_page());
    } else if (mode_ == DenseWeightsMode::Warmed) {
        warm();
    }
    advise_random_mapped();
    return true;
}

static const char * mode_flag(DenseWeightsMode m) {
    switch (m) {
    case DenseWeightsMode::Mmap:
        return "mmap";
    case DenseWeightsMode::Warmed:
        return "warm";
    case DenseWeightsMode::Anonymous:
        return "anon";
    case DenseWeightsMode::Pinned:
        return "ahwb";
    }
    return "?";
}

// Remove [a, b) from a sorted list of disjoint [first, second) ranges, splitting the one it falls in.
static void subtract_range(std::vector<std::pair<uint64_t, uint64_t>> & ranges, uint64_t a, uint64_t b) {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    out.reserve(ranges.size() + 1);
    for (const auto & r : ranges) {
        if (b <= r.first || a >= r.second) {
            out.push_back(r); // disjoint
            continue;
        }
        if (r.first < a) out.push_back({r.first, a});
        if (b < r.second) out.push_back({b, r.second});
    }
    ranges.swap(out);
}

// ── Oversized dense tensors: mmap'd under every mode ─────────────────────────────────
//
// A dense tensor bigger than the memory the kernel says is available cannot be resident under any
// mode: Anonymous/Pinned would ask for an allocation that fails or takes the process with it, and
// Warmed would sweep it through a page cache it cannot fit in, evicting itself as it goes. Until
// qwen4exp this could not happen — the largest dense tensor was an embedding or lm_head — but its
// n-gram table (`per_layer_token_embd`, ~28.8 GB at IQ4_NL) exceeds any phone's RAM while the graph
// gathers a few rows from it per token, so mmap is the only residency it can have. Such a tensor
// leaves the set entirely: not read, not rebound, not dropped, not warmed, and not sampled — a
// sensor that counted it would report a dense set that can never be resident. The rest of the
// dense weights still get the mode the run asked for.
//
// The bound is the kernel's own MemAvailable rather than a constant, because it is the one number
// that already accounts for what this device can still hand out; a tensor that FITS keeps its
// mode even when it is row-gathered, since demand-faulting a table that fits costs more in prefill
// than reading it once sequentially at load (measured: token_embd left mmap'd lost 16 % decode).
void DenseWeights::hold_back_oversized() {
    mapped_.clear();
    const uint64_t avail = pio::mem_available_bytes();
    if (!avail) return; // unmeasured on this platform: hold nothing back, as before
    std::vector<DenseTensorRef> keep;
    keep.reserve(tensors_.size());
    uint64_t held = 0;
    for (const DenseTensorRef & d : tensors_) {
        if (d.size <= avail) {
            keep.push_back(d);
            continue;
        }
        mapped_.push_back(d);
        held += d.size;
        if (d.file_idx >= 0 && (size_t) d.file_idx < ranges_.size())
            subtract_range(ranges_[(size_t) d.file_idx], d.file_off, d.file_off + d.size);
    }
    tensors_ = std::move(keep);
    if (held)
        std::fprintf(stderr,
                     "bmoe: dense-weights=%s — %llu MiB in %zu tensor(s) exceeds the %llu MiB available: left "
                     "mmap'd with random-access advice, outside the warm sweep and the residency sensor\n",
                     mode_flag(mode_), (unsigned long long) (held >> 20), mapped_.size(),
                     (unsigned long long) (avail >> 20));
}

void DenseWeights::set_row_gathered(std::vector<DenseTensorRef> tables, uint64_t budget_bytes) {
    row_pending_ = std::move(tables);
    row_budget_ = budget_bytes;
}

IRowSource * DenseWeights::row_source() const {
    return rows_ && !rows_->empty() ? rows_.get() : nullptr;
}

RowSourceStats DenseWeights::row_stats() const {
    return rows_ ? rows_->stats() : RowSourceStats{};
}

// ── Row-gathered tables: bound to reserved space, served from flash ──────────────────
//
// Runs after hold_back_oversized, which is what restricts this to tables that COULD have been
// resident. That is deliberate rather than incidental: materialize() — the fallback for a graph
// shape the capture pass never saw — has to be able to pull the whole table in, and a table larger
// than memory could not honour it. An oversized row-gathered table keeps the mmap policy that was
// measured for it.
//
// A table only leaves the resident set once the takeover has actually succeeded, so a failed
// reservation or a reader that will not open costs nothing but the log line.
void DenseWeights::take_row_gathered() {
    if (row_pending_.empty()) return;
    // Intersect with what is still resident-eligible, by tensor identity: the caller's list was
    // built from the same capture, but hold_back_oversized may have taken some of it already.
    std::vector<DenseTensorRef> take;
    for (const DenseTensorRef & want : row_pending_)
        for (const DenseTensorRef & have : tensors_)
            if (have.tensor == want.tensor) {
                take.push_back(have);
                break;
            }
    row_pending_.clear();
    if (take.empty()) return;

    rows_.reset(new RowStream());
    if (!rows_->init(take, paths_, align_, row_budget_) || rows_->empty()) {
        rows_.reset();
        return; // the tables keep the mode the run asked for
    }

    std::vector<DenseTensorRef> keep;
    keep.reserve(tensors_.size());
    uint64_t freed = 0;
    for (const DenseTensorRef & d : tensors_) {
        bool taken = false;
        for (const DenseTensorRef & t : take)
            if (t.tensor == d.tensor) {
                taken = true;
                break;
            }
        if (!taken) {
            keep.push_back(d);
            continue;
        }
        freed += d.size;
        if (d.file_idx >= 0 && (size_t) d.file_idx < ranges_.size())
            subtract_range(ranges_[(size_t) d.file_idx], d.file_off, d.file_off + d.size);
    }
    tensors_ = std::move(keep);
    std::fprintf(stderr, "bmoe: dense-weights=%s — %llu MiB of row-gathered table(s) left out of the resident set\n",
                 mode_flag(mode_), (unsigned long long) (freed >> 20));
}

void DenseWeights::advise_random_mapped() {
    if (mapped_.empty()) return;
    resolve_vmas();
    const size_t page = pio::vm_page();
    for (const DenseTensorRef & d : mapped_) {
        const char * a = addr_of(d.file_idx, d.file_off);
        if (!a) continue;
        // Inward to whole pages, as drop_mmap_copies does: the edge pages are shared with neighbours
        // that keep their own access pattern, and on a table this size two pages are nothing.
        uintptr_t a0 = ((uintptr_t) a + page - 1) & ~(uintptr_t) (page - 1);
        uintptr_t a1 = ((uintptr_t) a + d.size) & ~(uintptr_t) (page - 1);
        if (a1 > a0) pio::vm_advise_random((void *) a0, (size_t) (a1 - a0));
    }
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
        if (d.file_idx < 0 || d.file_idx >= (int) readers_.size()) {
            std::fprintf(stderr, "bmoe: dense tensor points at shard %d of %zu\n", d.file_idx, readers_.size());
            return false;
        }
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
            if (readers_[(size_t) d.file_idx]->read(0, (char *) buf + done, d.file_off + done, n) < 0) return false;
            done += n;
        }
        d.tensor->data = buf; // rebind the model weight onto its private copy
        total += d.size;
    }
    std::fprintf(stderr, "bmoe: dense-weights=%s — %llu MiB in %zu %s buffers\n", pinned ? "ahwb" : "anon",
                 (unsigned long long) (total >> 20), buf_sz_.size(), pinned ? "pinned" : "anon");
    return true;
}

// Per-shard VMA resolution for every consumer that must turn (file_idx, offset) into an address.
// llama.cpp maps each shard of a split model separately, so the lookup is per shard basename.
void DenseWeights::resolve_vmas() {
    if (vmas_tried_) return;
    vmas_tried_ = true;
    vmas_.resize(basenames_.size());
    for (size_t s = 0; s < basenames_.size(); ++s)
        pio::file_mapped_regions(basenames_[s].c_str(), vmas_[s]);
}

const char * DenseWeights::addr_of(int file_idx, uint64_t off) const {
    if (file_idx < 0 || file_idx >= (int) vmas_.size()) return nullptr;
    for (const auto & v : vmas_[file_idx]) {
        const uint64_t span = (uint64_t) (v.end - v.start);
        if (off >= v.file_offset && off < v.file_offset + span) return (const char *) v.start + (off - v.file_offset);
    }
    return nullptr;
}

// Hand the mmap copies back. The capture warm-up decode faulted these dense pages in mmap-resident,
// and read_anonymous has just copied them into anon buffers and rebound every tensor — so the file-
// backed pages are referenced by nobody. Left alone they sit resident until reclaim, doubling the
// dense footprint at the worst moment (right before prefill). Drop them with MADV_DONTNEED: a clean
// read-only mapping, so nothing is lost and nothing will refault the range. Best-effort — needs
// /proc/self/maps to turn a file offset into an address; where that is unreadable the pages stay.
void DenseWeights::drop_mmap_copies(size_t page) {
    resolve_vmas();
    uint64_t dropped = 0;
    for (const DenseTensorRef & d : tensors_) {
        if (!d.tensor || d.size == 0) continue;
        const char * a = addr_of(d.file_idx, d.file_off);
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
    const size_t chunk = 8ull << 20;
    void * buf = pio::alloc_aligned(align_, chunk);
    if (!buf) return;
    const auto t0 = clock_t_::now();
    uint64_t warmed = 0;
    bool all_ok = true; // sticky, for the report only: a failed shard must not abort the others
    for (size_t s = 0; s < paths_.size() && s < ranges_.size(); ++s) {
        pio::fd_t fd = pio::open_read(paths_[s].c_str(), false);
        if (!pio::fd_ok(fd)) {
            all_ok = false; // best-effort: that shard's pages just stay cold
            continue;
        }
        bool shard_ok = true;
        for (const auto & r : ranges_[s]) {
            for (uint64_t a = r.first; a < r.second && shard_ok;) {
                const long long got = pio::pread_at(fd, buf, (size_t) std::min<uint64_t>(chunk, r.second - a), a);
                if (got <= 0) {
                    shard_ok = all_ok = false;
                    break;
                }
                a += (uint64_t) got;
                warmed += (uint64_t) got;
            }
        }
        pio::close_fd(fd);
    }
    const bool ok = all_ok;
    pio::aligned_free(buf);
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

// Mmap/Warmed: the weights are llama.cpp's mmaps of the gguf shards; a dense (shard, offset) becomes
// an address through the VMA that backs it (/proc/self/maps), which an app may read, being its own.
// Probe sample_pages points spread evenly across the dense bytes of ALL shards, one page each, so the
// fraction keeps meaning "of the whole dense set" whatever the shard layout.
void DenseWeights::sample_mmap(size_t page) {
    if (ranges_.empty()) return;
    resolve_vmas();

    uint64_t total = 0;
    for (const auto & shard : ranges_)
        for (const auto & r : shard)
            total += r.second - r.first;
    if (total == 0) return;

    size_t sampled = 0, resident = 0;
    for (int k = 0; k < sample_pages; ++k) {
        uint64_t target = (total * (uint64_t) k) / (uint64_t) sample_pages;
        int fi = -1; // found shard; a dense offset of 0 is valid, so the sentinel is the index
        uint64_t off = 0;
        for (size_t s = 0; s < ranges_.size() && fi < 0; ++s) {
            for (const auto & r : ranges_[s]) {
                const uint64_t len = r.second - r.first;
                if (target < len) {
                    fi = (int) s;
                    off = r.first + target;
                    break;
                }
                target -= len;
            }
        }
        // Align the probe DOWN to its page: vm_resident_sample counts only pages fully inside the
        // range, so a single page handed in at an arbitrary offset would clip to nothing.
        if (const char * a = fi >= 0 ? addr_of(fi, off) : nullptr) {
            const char * pg = (const char *) ((uintptr_t) a & ~(uintptr_t) (page - 1));
            pio::vm_resident_sample(pg, page, &sampled, &resident);
        }
    }
    resident_frac_ = sampled ? (double) resident / (double) sampled : -1.0;
}

void DenseWeights::shutdown() {
    if (rows_) rows_->shutdown();
    rows_.reset();
    for (void * b : bufs_)
        if (b) pio::aligned_free(b);
    for (pio::PinnedAlloc & p : pinned_)
        pio::pinned_free(&p);
    bufs_.clear();
    pinned_.clear();
    bases_.clear();
    buf_sz_.clear();
    tensors_.clear();
    readers_.clear();
}

} // namespace bmoe
