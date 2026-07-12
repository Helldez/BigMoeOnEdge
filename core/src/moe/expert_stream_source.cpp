#include "expert_stream_source.h"

#include "ggml.h"
#ifdef BMOE_HAVE_EXPERT_READY_HOOK
#include "ggml-cpu.h" // ggml_cpu_set_expert_ready_hook (fork-only)
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace bmoe {

using clock_t_ = std::chrono::steady_clock;

ExpertStreamSource::~ExpertStreamSource() {
    shutdown();
}

// ── init: allocate buffers, rebind expert tensors, start the read pool ──────────────
bool ExpertStreamSource::init(const std::string & gguf_path,
                              int n_expert,
                              std::vector<LayerExperts> layers,
                              const MoeStreamConfig & cfg) {
    if (active_) return false;
    if (n_expert <= 0) {
        std::fprintf(stderr, "bmoe: expert streaming needs a MoE model (n_expert=%d)\n", n_expert);
        return false;
    }

    n_expert_ = n_expert;
    layers_ = std::move(layers);
    n_layer_ = (int) layers_.size();
    o_direct_ = cfg.o_direct;
    load_all_ = cfg.load_all;
    overlap_ = cfg.overlap;
    cache_max_ = (size_t) std::max(0, cfg.cache_mb) * 1024ull * 1024ull;
    io_threads_ = std::max(1, std::min(MoeStreamConfig::io_threads_max, cfg.io_threads));

    // Largest full-tensor byte size per projection, over all bound layers → shared-slot
    // and bounce sizing. Absent projection slots (a fused layout uses fewer than max_exps
    // expert tensors) keep nb2 == 0, so their max_full stays 0 and they are skipped below.
    size_t max_full[MoeRecipe::max_exps] = {0, 0, 0};
    for (const LayerExperts & L : layers_) {
        if (!L.bound) continue;
        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            const size_t full = (size_t) L.proj[p].nb2 * (size_t) n_expert_;
            max_full[p] = std::max(max_full[p], full);
        }
    }
    size_t max_full_any = 0;
    for (int p = 0; p < MoeRecipe::max_exps; ++p)
        max_full_any = std::max(max_full_any, max_full[p]);
    if (max_full_any == 0) {
        std::fprintf(stderr, "bmoe: no MoE layers were bound\n");
        return false;
    }

    if (cache_max_ == 0) {
        // One shared slot per present projection, reused across layers (one layer computes
        // at a time). Rebind every bound layer's expert tensors onto them; only routed
        // slices are ever valid. Absent slots (max_full[p] == 0) get no buffer.
        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            if (max_full[p] == 0) continue;
            slot_[p] = pio::alloc_aligned(align_, max_full[p]);
            if (!slot_[p]) {
                std::fprintf(stderr, "bmoe: slot alloc %zu failed\n", max_full[p]);
                return false;
            }
            slot_sz_[p] = max_full[p];
        }
        for (LayerExperts & L : layers_) {
            if (!L.bound) continue;
            for (int p = 0; p < MoeRecipe::max_exps; ++p)
                if (L.proj[p].tensor) L.proj[p].tensor->data = slot_[p];
        }
    } else {
        // LRU cache: one reserved (address-only) buffer per (layer, projection). Physical
        // pages appear on the first miss and are released on eviction. mul_mat_id needs
        // each expert at its canonical offset e*nb2 inside tensor->data, so the buffers
        // live at fixed per-layer addresses; lazy commit keeps that affordable.
        page_ = pio::vm_page();
        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            lbuf_[p].assign(n_layer_, nullptr);
            lbuf_sz_[p].assign(n_layer_, 0);
        }
        for (int il = 0; il < n_layer_; ++il) {
            LayerExperts & L = layers_[il];
            if (!L.bound) continue;
            for (int p = 0; p < MoeRecipe::max_exps; ++p) {
                if (!L.proj[p].tensor) continue; // absent slot in a fused layout
                const size_t full = (size_t) L.proj[p].nb2 * (size_t) n_expert_;
                lbuf_[p][il] = pio::vm_reserve(full);
                if (!lbuf_[p][il]) {
                    std::fprintf(stderr, "bmoe: vm_reserve %zu failed (layer %d)\n", full, il);
                    return false;
                }
                lbuf_sz_[p][il] = full;
                L.proj[p].tensor->data = lbuf_[p][il];
            }
        }
        const size_t n_entry = (size_t) n_layer_ * n_expert_;
        cvalid_.assign(n_entry, 0);
        cstamp_.assign(n_entry, 0);
        cprev_.assign(n_entry, -1);
        cnext_.assign(n_entry, -1);
        chead_ = ctail_ = -1;
        cresident_ = 0;
        cgen_ = 0;
        chits_ = 0;
        clookups_ = 0;
    }

    // Read pool: a private fd + bounce per lane so concurrent preads never contend.
    const size_t max_slice = max_full_any / (size_t) n_expert_;
    const size_t bounce_cap = max_slice + 2 * align_;

    pio::fd_t primary = pio::open_read(gguf_path.c_str(), o_direct_);
    if (!pio::fd_ok(primary) && o_direct_) {
        o_direct_ = false;
        primary = pio::open_read(gguf_path.c_str(), false);
    }
    if (!pio::fd_ok(primary)) {
        std::fprintf(stderr, "bmoe: open %s failed\n", gguf_path.c_str());
        return false;
    }
    fsize_ = pio::file_size(primary);

    // Verify O_DIRECT actually returns correct bytes on this storage. On some emulated / FUSE-backed
    // volumes (e.g. an app-private dir under /storage/emulated) the open SUCCEEDS but direct reads
    // return garbage, silently corrupting expert weights → nonsense output. Compare one aligned
    // block read directly against the same block read buffered; on any mismatch, fall back to
    // buffered I/O for this file. On real filesystems (adb-pushed models, desktop) the two match
    // and O_DIRECT is kept.
    if (o_direct_ && fsize_ >= (uint64_t) align_) {
        uint64_t voff = (fsize_ / 2) & ~(uint64_t) (align_ - 1);
        if (voff + align_ > fsize_) voff = 0;
        void * dbuf = pio::alloc_aligned(align_, align_);
        pio::fd_t vfd = pio::open_read(gguf_path.c_str(), false);
        if (dbuf && pio::fd_ok(vfd)) {
            std::vector<char> bbuf(align_);
            const long long want = (long long) align_;
            const long long gd = pio::pread_at(primary, dbuf, align_, voff);
            const long long gb = pio::pread_at(vfd, bbuf.data(), align_, voff);
            const bool bad = gd != want || gb != want || std::memcmp(dbuf, bbuf.data(), align_) != 0;
            if (bad) {
                std::fprintf(stderr, "bmoe: O_DIRECT returns wrong data on this storage — using buffered I/O\n");
                o_direct_ = false;
                pio::close_fd(primary);
                primary = pio::open_read(gguf_path.c_str(), false);
            }
        }
        if (dbuf) pio::aligned_free(dbuf);
        if (pio::fd_ok(vfd)) pio::close_fd(vfd);
        if (!pio::fd_ok(primary)) {
            std::fprintf(stderr, "bmoe: reopen after O_DIRECT check failed\n");
            return false;
        }
    }

    fds_.assign(io_threads_, pio::fd_invalid);
    fds_buf_.assign(io_threads_, pio::fd_invalid);
    bounces_.assign(io_threads_, nullptr);
    bounce_sz_.assign(io_threads_, 0);
    for (int lane = 0; lane < io_threads_; ++lane) {
        fds_[lane] = (lane == 0) ? primary : pio::open_read(gguf_path.c_str(), o_direct_);
        fds_buf_[lane] = o_direct_ ? pio::open_read(gguf_path.c_str(), false) : pio::fd_invalid;
        if (!pio::fd_ok(fds_[lane])) {
            std::fprintf(stderr, "bmoe: lane %d open failed\n", lane);
            return false;
        }
        bounces_[lane] = pio::alloc_aligned(align_, bounce_cap);
        if (!bounces_[lane]) {
            std::fprintf(stderr, "bmoe: lane %d bounce alloc failed\n", lane);
            return false;
        }
        bounce_sz_[lane] = bounce_cap;
    }

    seen_.assign(n_expert_, 0);
    jobs_.reserve((size_t) n_expert_ * MoeRecipe::max_exps);
    batch_gen_ = 0;
    next_idx_ = 0;
    done_cnt_ = 0;
    io_stop_ = false;
    io_err_.store(false);

    if (overlap_) {
        // One readiness cell per (projection, expert), and a map from each bound expert tensor
        // (the persistent model weight, stable across decodes) back to its (layer, projection)
        // so the hook can find the cell to wait on. Built after the rebind above.
        ready_ = std::vector<ReadyFlag>((size_t) MoeRecipe::max_exps * (size_t) n_expert_);
        async_gen_.store(0);
        cur_il_.store(-1);
        fatal_.store(false);
        stall_ns_.store(0);
        batch_flag_gen_ = 0;
        staged_.reserve(n_expert_);
        texp_.clear();
        texp_.reserve((size_t) n_layer_ * MoeRecipe::max_exps);
        for (int il = 0; il < n_layer_; ++il) {
            const LayerExperts & L = layers_[il];
            if (!L.bound) continue;
            for (int p = 0; p < MoeRecipe::max_exps; ++p)
                if (L.proj[p].tensor) texp_[(const void *) L.proj[p].tensor] = ((uint32_t) il << 8) | (uint32_t) p;
        }
    }

    active_ = true;
    // Serial: lane 0 is the calling thread (it drains inline), workers own lanes 1..N-1.
    // Overlap: the caller never drains — every lane 0..N-1 gets a worker so reads proceed
    // while the compute threads run the FFN and block per expert on the readiness flags.
    const int first_worker_lane = overlap_ ? 0 : 1;
    for (int lane = first_worker_lane; lane < io_threads_; ++lane)
        io_pool_.emplace_back(&ExpertStreamSource::io_worker, this, lane);

    std::fprintf(stderr, "bmoe: expert streaming ON  n_expert=%d o_direct=%d io_threads=%d cache=%zu MiB\n", n_expert_,
                 (int) o_direct_, io_threads_, cache_max_ >> 20);
    return true;
}

// ── one aligned slice read on a lane ────────────────────────────────────────────────
bool ExpertStreamSource::read_slice(int lane, void * dst, uint64_t off, uint64_t nbytes) {
    if (nbytes == 0) return true;
    const uint64_t a0 = off & ~(uint64_t) (align_ - 1);
    const uint64_t a1 = (off + nbytes + align_ - 1) & ~(uint64_t) (align_ - 1);
    const size_t len = (size_t) (a1 - a0);
    if (bounce_sz_[lane] < len) {
        if (bounces_[lane]) pio::aligned_free(bounces_[lane]);
        bounces_[lane] = pio::alloc_aligned(align_, len);
        if (!bounces_[lane]) {
            std::fprintf(stderr, "bmoe: bounce realloc %zu failed\n", len);
            return false;
        }
        bounce_sz_[lane] = len;
    }
    char * b = (char *) bounces_[lane];
    const pio::fd_t fd = fds_[lane];
    const pio::fd_t fd_buf = fds_buf_[lane];
    const uint64_t read_end = (fsize_ && a1 > fsize_) ? fsize_ : a1;
    const uint64_t bulk_end = o_direct_ ? (read_end & ~(uint64_t) (align_ - 1)) : read_end;

    const auto t0 = clock_t_::now();
    for (uint64_t a = a0; a < bulk_end;) {
        long long got = pio::pread_at(fd, b + (a - a0), (size_t) (bulk_end - a), a);
        if (got <= 0) {
            std::fprintf(stderr, "bmoe: pread failed at %llu\n", (unsigned long long) a);
            return false;
        }
        a += (uint64_t) got;
    }
    for (uint64_t a = bulk_end; a < read_end;) { // sub-alignment EOF tail via the buffered fd
        long long got = pio::pread_at(pio::fd_ok(fd_buf) ? fd_buf : fd, b + (a - a0), (size_t) (read_end - a), a);
        if (got <= 0) {
            std::fprintf(stderr, "bmoe: tail pread failed at %llu\n", (unsigned long long) a);
            return false;
        }
        a += (uint64_t) got;
    }
    const auto t1 = clock_t_::now();
    io_syscall_ns_.fetch_add((long long) std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    std::memcpy(dst, b + (off - a0), (size_t) nbytes);
    read_bytes_.fetch_add((long long) (read_end - a0));
    return true;
}

void ExpertStreamSource::io_drain(int lane, uint64_t my_gen) {
    for (;;) {
        size_t i;
        {
            std::lock_guard<std::mutex> lk(io_mtx_);
            if (batch_gen_ != my_gen || next_idx_ >= batch_njobs_) return;
            i = next_idx_++;
        }
        const IoJob & j = jobs_[i];
        if (!read_slice(lane, j.dst, j.off, j.nbytes)) {
            io_err_.store(true);
            if (overlap_) fatal_.store(true, std::memory_order_release);
        }
        // Overlap: publish this expert's readiness (batch_flag_gen_ is the async_gen_ of the
        // in-flight batch, fixed until it fully drains) and wake any compute thread blocked on
        // it. Notify even on failure so a straggler wakes and observes fatal_ instead of hanging.
        if (j.flag >= 0) {
            ready_[(size_t) j.flag].gen.store(batch_flag_gen_, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(ready_mtx_);
                ready_cv_.notify_all();
            }
        }
        std::lock_guard<std::mutex> lk(io_mtx_);
        if (++done_cnt_ == batch_njobs_) io_cv_done_.notify_all();
    }
}

void ExpertStreamSource::io_worker(int lane) {
    uint64_t seen = 0;
    for (;;) {
        uint64_t g;
        {
            std::unique_lock<std::mutex> lk(io_mtx_);
            io_cv_.wait(lk, [&] { return io_stop_ || batch_gen_ > seen; });
            if (io_stop_) return;
            g = batch_gen_;
            seen = g;
        }
        io_drain(lane, g);
    }
}

// ── LRU plumbing ────────────────────────────────────────────────────────────────────
void ExpertStreamSource::lru_unlink(int32_t id) {
    int32_t pv = cprev_[id], nx = cnext_[id];
    if (pv != -1)
        cnext_[pv] = nx;
    else
        chead_ = nx;
    if (nx != -1)
        cprev_[nx] = pv;
    else
        ctail_ = pv;
    cprev_[id] = cnext_[id] = -1;
}
void ExpertStreamSource::lru_push_front(int32_t id) {
    cprev_[id] = -1;
    cnext_[id] = chead_;
    if (chead_ != -1)
        cprev_[chead_] = id;
    else
        ctail_ = id;
    chead_ = id;
}
size_t ExpertStreamSource::entry_bytes(int il) const {
    const LayerExperts & L = layers_[il];
    size_t bytes = 0;
    for (int p = 0; p < MoeRecipe::max_exps; ++p)
        bytes += (size_t) L.proj[p].nb2; // absent slots contribute 0
    return bytes;
}
void ExpertStreamSource::evict_tail() {
    const int32_t id = ctail_;
    const int il = id / n_expert_, e = id % n_expert_;
    for (int p = 0; p < MoeRecipe::max_exps; ++p) {
        const uint64_t slice = layers_[il].proj[p].nb2;
        if (slice == 0) continue; // absent slot in a fused layout
        char * s = (char *) lbuf_[p][il] + (uint64_t) e * slice;
        uintptr_t a0 = ((uintptr_t) s + page_ - 1) & ~(uintptr_t) (page_ - 1);
        uintptr_t a1 = ((uintptr_t) s + slice) & ~(uintptr_t) (page_ - 1);
        if (a1 > a0) pio::vm_evict((void *) a0, (size_t) (a1 - a0));
    }
    cvalid_[id] = 0;
    cresident_ -= entry_bytes(il);
    lru_unlink(id);
}

// ── load: stage routed experts, read the batch, evict cold entries to budget ────────
bool ExpertStreamSource::load_layer(int il, const int32_t * ids, int n_ids) {
    if (!active_ || il < 0 || il >= n_layer_ || !layers_[il].bound || !ids || n_ids <= 0) return false;
    if (overlap_) return load_layer_async(il, ids, n_ids);
    LayerExperts & L = layers_[il];
    cgen_++;
    jobs_.clear();

    // Cache-management work (vm commit + LRU bookkeeping, then eviction below) is timed into
    // mgmt_ns_ so the metrics can split it out of the compute residual instead of hiding it there.
    const auto tm0 = clock_t_::now();

    auto stage = [&](int e) -> bool {
        if (cache_max_ == 0) {
            for (int p = 0; p < MoeRecipe::max_exps; ++p) {
                const uint64_t slice = L.proj[p].nb2;
                if (slice == 0) continue; // absent slot in a fused layout
                jobs_.push_back(
                    {(char *) slot_[p] + (uint64_t) e * slice, L.proj[p].file_off + (uint64_t) e * slice, slice});
            }
            return true;
        }
        const int32_t id = il * n_expert_ + e;
        clookups_++;
        cstamp_[id] = cgen_;
        if (cvalid_[id]) {
            chits_++;
            lru_unlink(id);
            lru_push_front(id);
            return true;
        }
        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            const uint64_t slice = L.proj[p].nb2;
            if (slice == 0) continue; // absent slot in a fused layout
            char * dst = (char *) lbuf_[p][il] + (uint64_t) e * slice;
            uintptr_t a0 = (uintptr_t) dst & ~(uintptr_t) (page_ - 1);
            uintptr_t a1 = ((uintptr_t) dst + slice + page_ - 1) & ~(uintptr_t) (page_ - 1);
            if (!pio::vm_commit((void *) a0, (size_t) (a1 - a0))) {
                std::fprintf(stderr, "bmoe: commit failed\n");
                return false;
            }
            jobs_.push_back({dst, L.proj[p].file_off + (uint64_t) e * slice, slice});
        }
        cvalid_[id] = 1;
        cresident_ += entry_bytes(il);
        lru_push_front(id);
        return true;
    };

    std::fill(seen_.begin(), seen_.end(), (uint8_t) 0);
    for (int i = 0; i < n_ids; ++i) {
        const int e = load_all_ ? (i < n_expert_ ? i : -1) : ids[i];
        if (e < 0 || e >= n_expert_ || seen_[e]) continue;
        seen_[e] = 1;
        if (!stage(e)) return false;
    }
    if (load_all_) {
        for (int e = 0; e < n_expert_; ++e) {
            if (seen_[e]) continue;
            seen_[e] = 1;
            if (!stage(e)) return false;
        }
    }

    const size_t njobs = jobs_.size();
    mgmt_ns_.fetch_add((long long) std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - tm0).count());
    const auto t0 = clock_t_::now();
    if (io_threads_ <= 1 || njobs <= 1) {
        for (size_t i = 0; i < njobs; ++i) {
            const IoJob & j = jobs_[i];
            if (!read_slice(0, j.dst, j.off, j.nbytes)) return false;
        }
    } else {
        uint64_t my_gen;
        {
            std::lock_guard<std::mutex> lk(io_mtx_);
            batch_njobs_ = njobs;
            next_idx_ = 0;
            done_cnt_ = 0;
            io_err_.store(false);
            my_gen = ++batch_gen_;
        }
        io_cv_.notify_all();
        io_drain(0, my_gen);
        {
            std::unique_lock<std::mutex> lk(io_mtx_);
            io_cv_done_.wait(lk, [&] { return done_cnt_ == njobs || io_stop_; });
        }
        if (io_err_.load()) return false;
    }
    read_ns_.fetch_add((long long) std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - t0).count());

    if (cache_max_) {
        const auto te0 = clock_t_::now();
        while (cresident_ > cache_max_ && ctail_ != -1 && cstamp_[ctail_] != cgen_)
            evict_tail();
        mgmt_ns_.fetch_add(
            (long long) std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - te0).count());
    }
    return true;
}

// ── overlap load: publish the routed reads, mark readiness, return without waiting ──────
//
// The bookkeeping is identical to the serial path but split in two: cache/LRU accounting is
// per-EXPERT (a hit resolves all of an expert's projections at once), while the jobs are
// emitted PROJECTION-MAJOR (all gate slices, then all up, then all down). mul_mat_id consumes
// the projections in that order, so projection-major reads feed the kernel roughly in the
// order it blocks on them, minimising stalls. Correctness does not depend on the order — the
// readiness flags gate each expert regardless — only latency does.
bool ExpertStreamSource::load_layer_async(int il, const int32_t * ids, int n_ids) {
    LayerExperts & L = layers_[il];

    // 1. Drain the previous batch fully before reusing jobs_/the flags. This is the eviction
    //    safety guarantee (no worker still reading an about-to-be-evicted page) and it covers
    //    load_all, whose surplus jobs no hook ever waits on. Serial's epilogue waited here too.
    {
        std::unique_lock<std::mutex> lk(io_mtx_);
        io_cv_done_.wait(lk, [&] { return done_cnt_ == batch_njobs_ || io_stop_; });
    }
    if (fatal_.load(std::memory_order_acquire)) return false;

    // Cache-management work below (dedup/sort, vm commit, LRU bookkeeping and the eviction at
    // the end) runs on the eval thread and would otherwise hide inside the compute residual. Time
    // it into mgmt_ns_ so the metrics can surface it. The drain wait above is I/O, not mgmt.
    const auto tm0 = clock_t_::now();

    // 2. New generation for this layer. A flag counts as ready only once its gen matches.
    const uint32_t gen = async_gen_.fetch_add(1, std::memory_order_relaxed) + 1;
    cur_il_.store(il, std::memory_order_relaxed);
    cgen_++;
    jobs_.clear();

    auto mark_ready = [&](int p, int e) {
        ready_[(size_t) p * (size_t) n_expert_ + (size_t) e].gen.store(gen, std::memory_order_release);
    };

    // 3a. Dedup + sort ascending (matches the hook's ascending expert visitation order).
    std::fill(seen_.begin(), seen_.end(), (uint8_t) 0);
    staged_.clear();
    for (int i = 0; i < n_ids; ++i) {
        const int e = load_all_ ? (i < n_expert_ ? i : -1) : ids[i];
        if (e < 0 || e >= n_expert_ || seen_[e]) continue;
        seen_[e] = 1;
        staged_.push_back(e);
    }
    if (load_all_) {
        for (int e = 0; e < n_expert_; ++e) {
            if (seen_[e]) continue;
            seen_[e] = 1;
            staged_.push_back(e);
        }
    }
    std::sort(staged_.begin(), staged_.end());

    if (cache_max_ == 0) {
        // Cache off: every (projection, expert) is a fresh read into the shared full-size slot
        // at its canonical offset e*slice. Emit projection-major.
        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            const uint64_t slice = L.proj[p].nb2;
            if (slice == 0) continue; // absent slot in a fused layout
            for (int e : staged_) {
                const int32_t flag = (int32_t) ((size_t) p * (size_t) n_expert_ + (size_t) e);
                jobs_.push_back(
                    {(char *) slot_[p] + (uint64_t) e * slice, L.proj[p].file_off + (uint64_t) e * slice, slice, flag});
            }
        }
    } else {
        // Cache on: per-expert LRU bookkeeping (hit → mark all projections ready now; miss →
        // commit the pages and remember it). Then emit the misses' jobs projection-major.
        seen_.assign(seen_.size(), 0); // reuse as a per-staged miss marker keyed by expert
        for (int e : staged_) {
            const int32_t id = il * n_expert_ + e;
            clookups_++;
            cstamp_[id] = cgen_;
            if (cvalid_[id]) {
                chits_++;
                lru_unlink(id);
                lru_push_front(id);
                for (int p = 0; p < MoeRecipe::max_exps; ++p)
                    if (L.proj[p].nb2) mark_ready(p, e);
                continue;
            }
            for (int p = 0; p < MoeRecipe::max_exps; ++p) {
                const uint64_t slice = L.proj[p].nb2;
                if (slice == 0) continue;
                char * dst = (char *) lbuf_[p][il] + (uint64_t) e * slice;
                uintptr_t a0 = (uintptr_t) dst & ~(uintptr_t) (page_ - 1);
                uintptr_t a1 = ((uintptr_t) dst + slice + page_ - 1) & ~(uintptr_t) (page_ - 1);
                if (!pio::vm_commit((void *) a0, (size_t) (a1 - a0))) {
                    std::fprintf(stderr, "bmoe: commit failed\n");
                    fatal_.store(true, std::memory_order_release);
                    return false;
                }
            }
            cvalid_[id] = 1;
            cresident_ += entry_bytes(il);
            lru_push_front(id);
            seen_[e] = 1; // this expert missed → its projections need reads
        }
        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            const uint64_t slice = L.proj[p].nb2;
            if (slice == 0) continue;
            for (int e : staged_) {
                if (!seen_[e]) continue; // cache hit, already marked ready
                const int32_t flag = (int32_t) ((size_t) p * (size_t) n_expert_ + (size_t) e);
                jobs_.push_back({(char *) lbuf_[p][il] + (uint64_t) e * slice,
                                 L.proj[p].file_off + (uint64_t) e * slice, slice, flag});
            }
        }
    }

    // 4. Publish the batch and return immediately — no drain. Workers fill the slices and
    //    flip the flags as they go; the compute threads block on those flags via the hook.
    {
        std::lock_guard<std::mutex> lk(io_mtx_);
        batch_njobs_ = jobs_.size();
        next_idx_ = 0;
        done_cnt_ = 0;
        io_err_.store(false);
        batch_flag_gen_ = gen;
        ++batch_gen_;
    }
    io_cv_.notify_all();

    // 5. Evict cold entries to budget. Safe to run concurrently with this batch's reads: step 1
    //    guaranteed no stale in-flight jobs, and current-gen entries (cstamp_ == cgen_) — the
    //    ones just staged — are never chosen, so eviction only releases pages nobody is reading.
    if (cache_max_) {
        while (cresident_ > cache_max_ && ctail_ != -1 && cstamp_[ctail_] != cgen_)
            evict_tail();
    }
    mgmt_ns_.fetch_add((long long) std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - tm0).count());
    return true;
}

// Static trampoline for the process-global fork hook → member.
void ExpertStreamSource::c_expert_ready(const ggml_tensor * src0, int expert, void * user_data) {
    static_cast<ExpertStreamSource *>(user_data)->on_expert_ready(src0, expert);
}

// Called from EVERY compute thread, for each routed expert, before it reads that expert's rows.
// Blocks until the expert's slice for the layer in flight is resident (or the run goes fatal).
void ExpertStreamSource::on_expert_ready(const ggml_tensor * src0, int expert) {
    auto it = texp_.find((const void *) src0);
    if (it == texp_.end()) return; // not one of our streamed expert tensors
    const uint32_t packed = it->second;
    const int il = (int) (packed >> 8);
    const int p = (int) (packed & 0xff);
    // Graph order guarantees exactly one layer's experts are staged at a time. A mismatch means
    // the kernel is asking for a layer we did not stage — fail fast rather than wait forever.
    if (il != cur_il_.load(std::memory_order_relaxed) || expert < 0 || expert >= n_expert_) {
        fatal_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(ready_mtx_);
            ready_cv_.notify_all();
        }
        return;
    }
    const size_t idx = (size_t) p * (size_t) n_expert_ + (size_t) expert;
    const uint32_t want = async_gen_.load(std::memory_order_relaxed);
    if (ready_[idx].gen.load(std::memory_order_acquire) == want) return; // already resident

    const auto t0 = clock_t_::now();
    // Short spin first: a slice usually lands within microseconds, cheaper than a syscall.
    for (int s = 0; s < 2048; ++s) {
        if (ready_[idx].gen.load(std::memory_order_acquire) == want || fatal_.load(std::memory_order_acquire)) {
            stall_ns_.fetch_add(
                (long long) std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - t0).count());
            return;
        }
        std::this_thread::yield();
    }
    {
        std::unique_lock<std::mutex> lk(ready_mtx_);
        ready_cv_.wait(lk, [&] {
            return ready_[idx].gen.load(std::memory_order_acquire) == want || fatal_.load(std::memory_order_acquire);
        });
    }
    stall_ns_.fetch_add((long long) std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - t0).count());
}

void ExpertStreamSource::enable_overlap_hook() {
#ifdef BMOE_HAVE_EXPERT_READY_HOOK
    ggml_cpu_set_expert_ready_hook(&ExpertStreamSource::c_expert_ready, this);
    hook_registered_ = true;
#endif
}

IExpertSource::Stats ExpertStreamSource::stats() const {
    Stats s;
    s.read_bytes = (uint64_t) read_bytes_.load();
    // Serial: read_ns_ is the wall time the caller was blocked in the read phase. Overlap: the
    // caller never blocks, so "I/O time" is instead the summed lane-busy time (io_syscall_ns_),
    // which the runtime reports as lane-busy per token rather than as a slice of wall time.
    s.read_seconds = (overlap_ ? io_syscall_ns_.load() : read_ns_.load()) / 1e9;
    s.mgmt_seconds = mgmt_ns_.load() / 1e9;
    s.cache_hits = chits_;
    s.cache_lookups = clookups_;
    s.cache_resident_bytes = (uint64_t) cresident_;
    s.stall_seconds = stall_ns_.load() / 1e9;
    return s;
}

void ExpertStreamSource::shutdown() {
    if (!active_) return;
#ifdef BMOE_HAVE_EXPERT_READY_HOOK
    // Unregister the process-global hook FIRST: after this no compute thread can enter
    // on_expert_ready and touch members we are about to tear down.
    if (hook_registered_) {
        ggml_cpu_set_expert_ready_hook(nullptr, nullptr);
        hook_registered_ = false;
    }
#endif
    // Wake any straggler blocked on a readiness flag so it observes fatal_ and unwinds.
    fatal_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(ready_mtx_);
        ready_cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(io_mtx_);
        io_stop_ = true;
    }
    io_cv_.notify_all();
    io_cv_done_.notify_all();
    for (auto & t : io_pool_)
        if (t.joinable()) t.join();
    io_pool_.clear();

    for (int p = 0; p < MoeRecipe::max_exps; ++p)
        if (slot_[p]) {
            pio::aligned_free(slot_[p]);
            slot_[p] = nullptr;
        }
    for (int p = 0; p < MoeRecipe::max_exps; ++p) {
        for (int il = 0; il < (int) lbuf_[p].size(); ++il)
            if (lbuf_[p][il]) pio::vm_release(lbuf_[p][il], lbuf_sz_[p][il]);
        lbuf_[p].clear();
        lbuf_sz_[p].clear();
    }
    for (void * b : bounces_)
        if (b) pio::aligned_free(b);
    bounces_.clear();
    bounce_sz_.clear();
    for (int lane = 0; lane < (int) fds_.size(); ++lane) {
        if (pio::fd_ok(fds_[lane])) pio::close_fd(fds_[lane]);
        if (pio::fd_ok(fds_buf_[lane])) pio::close_fd(fds_buf_[lane]);
    }
    fds_.clear();
    fds_buf_.clear();
    jobs_.clear();
    layers_.clear();
    active_ = false;
}

} // namespace bmoe
