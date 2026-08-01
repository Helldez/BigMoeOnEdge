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
#include <utility>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h> // _mm_pause for the readiness spin-wait
#endif

namespace bmoe {

using clock_t_ = std::chrono::steady_clock;

ExpertStreamSource::~ExpertStreamSource() {
    shutdown();
}

// ── init: allocate buffers, rebind expert tensors, start the read pool ──────────────
bool ExpertStreamSource::init(const std::vector<std::string> & shard_paths,
                              int n_expert,
                              std::vector<LayerExperts> layers,
                              const MoeStreamConfig & cfg) {
    if (active_) return false;
    if (n_expert <= 0) {
        std::fprintf(stderr, "bmoe: expert streaming needs a MoE model (n_expert=%d)\n", n_expert);
        return false;
    }
    if (shard_paths.empty()) {
        std::fprintf(stderr, "bmoe: expert streaming got no model file\n");
        return false;
    }

    n_expert_ = n_expert;
    layers_ = std::move(layers);
    n_layer_ = (int) layers_.size();
    load_all_ = cfg.load_all;
    overlap_ = cfg.overlap;
    two_wave_ = cfg.io_two_wave;
    prefetch_sync_ = cfg.prefetch_sync && !cfg.overlap; // serial only: overlap lane 0 is a worker
    // Under route-ahead the speculated ids of a layer ARE the ids its topk will commit, so the
    // demand load adopts that layer's in-flight speculation instead of discarding and re-reading
    // it. Off for every guessing predictor: adopting a wrong guess would make the load wait on
    // reads it does not need.
    spec_adopt_ = cfg.route_ahead > 0;
    cache_max_ = (size_t) std::max(0, cfg.cache_mb) * 1024ull * 1024ull;
    io_threads_ = std::max(1, std::min(MoeStreamConfig::io_threads_max, cfg.io_threads));
    page_ = pio::vm_page(); // the real OS page size, for the dense-residency probe in any cache mode

    // Largest full-tensor byte size per projection, over all bound layers → shared-slot
    // and bounce sizing. Absent projection slots (a fused layout uses fewer than max_exps
    // expert tensors) keep nb2 == 0, so their max_full stays 0 and they are skipped below.
    size_t max_full[MoeRecipe::max_exps] = {0, 0, 0};
    for (const LayerExperts & L : layers_) {
        if (!L.bound) continue;
        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            const size_t full = (size_t) L.proj[p].nb2 * (size_t) n_expert_;
            max_full[p] = std::max(max_full[p], full);
            total_expert_bytes_ += full; // every expert of every bound layer resident = the full set
        }
    }
    size_t max_full_any = 0;
    for (int p = 0; p < MoeRecipe::max_exps; ++p)
        max_full_any = std::max(max_full_any, max_full[p]);
    if (max_full_any == 0) {
        std::fprintf(stderr, "bmoe: no MoE layers were bound\n");
        return false;
    }

    // Auto budget: size the cache to the device once, now that the full expert-set size is known.
    // (validate() guarantees cache_mb == 0 here, so this is the sole source of cache_max_ when auto.)
    // One shot — the floor and the cap decide the budget here and are not kept: nothing re-sizes it
    // afterwards except an explicit set_cache_budget().
    if (cfg.cache_auto) {
        const size_t floor = (size_t) std::max(0, cfg.cache_floor_mb) * 1024ull * 1024ull; // RAM to leave free
        // Hard cap: the whole expert set, further capped by the user's ceiling when set.
        const size_t ceil = (size_t) std::max(0, cfg.cache_ceil_mb) * 1024ull * 1024ull;
        const size_t hard_cap = ceil > 0 ? std::min(ceil, total_expert_bytes_) : total_expert_bytes_;
        const size_t min_budget =
            std::min<size_t>((size_t) MoeStreamConfig::cache_min_mb * 1024ull * 1024ull, hard_cap);
        uint64_t avail = pio::mem_available_bytes();
        // Under anon/ahwb the dense weights are ABOUT to move from reclaimable page cache into
        // buffers the kernel cannot take back (dense_.init runs below). At this moment the kernel
        // still counts those pages as available, so a budget sized from the raw number plans the
        // dense set twice and overcommits by its whole size — enough to take the device down on a
        // model whose dense set is large (DeepSeek V4's is ~6.5 GiB). Budget as if the conversion
        // had already happened.
        if (cfg.dense_weights == DenseWeightsMode::Anonymous || cfg.dense_weights == DenseWeightsMode::Pinned) {
            uint64_t dense_pending = 0;
            for (const DenseTensorRef & d : dense_tensors_)
                if (d.tensor) dense_pending += d.size;
            const uint64_t deduct = std::min(avail, dense_pending);
            if (deduct > 0) {
                avail -= deduct;
                std::fprintf(stderr, "bmoe: cache auto — reserving %llu MiB for the dense-weight conversion\n",
                             (unsigned long long) (deduct / (1024 * 1024)));
            }
        }
        if (avail == 0) {
            cache_max_ = min_budget;
            std::fprintf(stderr, "bmoe: cache auto — available memory unknown, using the %zu MiB floor\n",
                         min_budget / (1024 * 1024));
        } else {
            size_t budget = avail > floor ? (size_t) (avail - floor) : 0;
            budget = std::max(budget, min_budget);
            budget = std::min(budget, hard_cap);
            cache_max_ = budget;
            std::fprintf(stderr,
                         "bmoe: cache auto — %llu MiB available, leaving %zu MiB free → %zu MiB budget"
                         " (cap %zu MiB)\n",
                         (unsigned long long) (avail / (1024 * 1024)), floor / (1024 * 1024),
                         cache_max_ / (1024 * 1024), hard_cap / (1024 * 1024));
        }
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
        // live at fixed per-layer addresses; lazy commit keeps that affordable. (page_ is already
        // set above: the dense-residency probe needs it in every cache mode, not just this one.)
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
        cspec_.assign(n_entry, 0);
        spec_remaining_.assign(n_entry, 0);
        ever_evicted_.assign(n_entry, 0);
        chead_ = ctail_ = -1;
        cresident_ = 0;
        cgen_ = 0;
        chits_ = 0;
        clookups_ = 0;
    }

    // Read pool: one reader per shard file, each owning a private fd + bounce per lane so concurrent
    // preads never contend, and the O_DIRECT request + its verify/fallback (per file — a shard set
    // could in principle straddle storage with different O_DIRECT behaviour). The dense-weights
    // loader opens its own readers, so its cache-bypass choice is independent of this one.
    const size_t max_slice = max_full_any / (size_t) n_expert_;
    const size_t bounce_cap = max_slice + 2 * align_;
    for (const std::string & sp : shard_paths) {
        readers_.push_back(std::unique_ptr<FileReader>(new FileReader()));
        if (!readers_.back()->open(sp, io_threads_, cfg.o_direct, align_, bounce_cap)) return false;
    }
    for (const LayerExperts & L : layers_) {
        if (!L.bound) continue;
        for (int p = 0; p < MoeRecipe::max_exps; ++p)
            if (L.proj[p].nb2 && (L.proj[p].file_idx < 0 || L.proj[p].file_idx >= (int) readers_.size())) {
                std::fprintf(stderr, "bmoe: expert tensor points at shard %d of %zu\n", L.proj[p].file_idx,
                             readers_.size());
                return false;
            }
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
                if (L.proj[p].tensor)
                    texp_.emplace_back((const void *) L.proj[p].tensor, ((uint32_t) il << 8) | (uint32_t) p);
        }
        std::sort(texp_.begin(), texp_.end());
    }

    // Hand the dense (non-expert) weights to their policy module: it warms them into the page cache,
    // or reads them into anon buffers and rebinds, and owns the residency sensor — all on the caller's
    // thread before the workers start (the Anonymous mode rebinds tensor->data). The dense byte ranges
    // are the complement of the expert ranges in the file, computed once here and shared by the warm
    // sweep and the sensor.
    {
        std::vector<std::vector<std::pair<uint64_t, uint64_t>>> exp(readers_.size());
        for (const LayerExperts & L : layers_) {
            if (!L.bound) continue;
            for (int p = 0; p < MoeRecipe::max_exps; ++p) {
                const uint64_t sz = (uint64_t) L.proj[p].nb2 * (uint64_t) n_expert_;
                if (sz) exp[L.proj[p].file_idx].push_back({L.proj[p].file_off, L.proj[p].file_off + sz});
            }
        }
        std::vector<std::vector<std::pair<uint64_t, uint64_t>>> ranges(readers_.size());
        for (size_t s = 0; s < readers_.size(); ++s)
            ranges[s] = DenseWeights::byte_ranges(std::move(exp[s]), readers_[s]->file_size());
        if (!dense_.init(cfg.dense_weights, shard_paths, align_, std::move(ranges), std::move(dense_tensors_))) {
            std::fprintf(stderr, "bmoe: dense-weights init failed\n");
            return false;
        }
    }

    active_ = true;
    // Serial: lane 0 is the calling thread (it drains inline), workers own lanes 1..N-1.
    // Overlap: the caller never drains — every lane 0..N-1 gets a worker so reads proceed
    // while the compute threads run the FFN and block per expert on the readiness flags.
    const int first_worker_lane = overlap_ ? 0 : 1;
    for (int lane = first_worker_lane; lane < io_threads_; ++lane)
        io_pool_.emplace_back(&ExpertStreamSource::io_worker, this, lane);

    // Each shard verified O_DIRECT for itself, so report the weakest: a metadata-only first shard
    // is too short to verify at all and would flatter the number for the shards carrying experts.
    bool all_direct = true;
    for (const auto & r : readers_)
        all_direct = all_direct && r->direct();
    std::fprintf(stderr, "bmoe: expert streaming ON  n_expert=%d o_direct=%d io_threads=%d cache=%zu MiB shards=%zu\n",
                 n_expert_, (int) all_direct, io_threads_, cache_max_ >> 20, readers_.size());
    return true;
}

// ── one aligned slice read on a lane ────────────────────────────────────────────────
// The bytes come from the reader; this wraps it with the domain the reader must not know about — the
// per-read I/O trace that attributes a read to its (layer, expert, projection). Latency is timed here
// so the row carries this read's own cost, not the reader's running total.
bool ExpertStreamSource::read_slice(int lane, const IoJob & j) {
    if (j.nbytes == 0) return true;
    const auto t0 = clock_t_::now();
    const long long window = readers_[(size_t) j.file]->read(lane, j.dst, j.off, j.nbytes);
    if (window < 0) return false;

    if (io_trace_on_) {
        const uint64_t lat_ns =
            (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - t0).count();
        IoTraceRow r;
        r.layer = j.layer;
        r.expert = j.expert;
        r.proj = (int8_t) j.proj;
        r.lane = (int8_t) lane;
        r.spec = j.spec;
        r.offset = j.off;
        r.req_bytes = j.nbytes;
        r.read_bytes = (uint64_t) window; // the aligned window pulled — what bandwidth is judged against
        r.latency_ns = lat_ns;
        std::lock_guard<std::mutex> lk(io_trace_mtx_);
        io_trace_rows_.push_back(r);
    }
    return true;
}

void ExpertStreamSource::set_io_trace(bool on) {
    std::lock_guard<std::mutex> lk(io_trace_mtx_);
    io_trace_on_ = on;
    io_trace_rows_.clear();
}

void ExpertStreamSource::take_io_trace_rows(std::vector<IoTraceRow> & out) {
    std::lock_guard<std::mutex> lk(io_trace_mtx_);
    out.swap(io_trace_rows_);
    io_trace_rows_.clear();
}

void ExpertStreamSource::io_drain(int lane, uint64_t my_gen) {
    for (;;) {
        IoJob j;
        {
            std::lock_guard<std::mutex> lk(io_mtx_);
            if (batch_gen_ != my_gen || next_idx_ >= batch_njobs_) return;
            // Copy under the lock: a two-wave publish appends to jobs_ mid-batch, and a vector
            // that reallocates would leave a reference taken here dangling.
            j = jobs_[next_idx_++];
        }
        if (!read_slice(lane, j)) {
            io_err_.store(true);
            if (overlap_) fatal_.store(true, std::memory_order_release);
        }
        // Overlap: publish this expert's readiness (batch_flag_gen_ is the async_gen_ of the
        // in-flight batch, fixed until it fully drains) and wake any compute thread blocked on
        // it. Notify even on failure so a straggler wakes and observes fatal_ instead of hanging.
        if (j.flag >= 0) {
            // Publish first, then look for waiters. on_expert_ready registers itself before its own
            // last look at the flag, and both operations are seq_cst, so the two cannot miss each
            // other: either the compute thread sees the flag and never sleeps, or it is counted here
            // and gets woken. With nobody waiting — the common case, since a slice usually lands
            // inside the spin — this costs one atomic load instead of a mutex plus a notify_all that
            // woke EVERY compute thread blocked on ANY expert to re-check its own predicate.
            ready_[(size_t) j.flag].gen.store(batch_flag_gen_, std::memory_order_seq_cst);
            if (ready_waiters_.load(std::memory_order_seq_cst) != 0) {
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
        {
            std::unique_lock<std::mutex> lk(io_mtx_);
            // `next_idx_ < batch_njobs_` admits a batch that GREW in the same generation (a
            // two-wave publish): a worker that drained wave one and left has seen == batch_gen_,
            // so the gen comparison alone would never bring it back for wave two.
            io_cv_.wait(lk, [&] {
                return io_stop_ || batch_gen_ > seen || next_idx_ < batch_njobs_ || spec_next_ < spec_jobs_.size();
            });
            if (io_stop_) return;
        }
        uint64_t g;
        {
            std::lock_guard<std::mutex> lk(io_mtx_);
            g = batch_gen_;
        }
        if (g > seen) seen = g;
        // Real batch first — it is latency-critical. With nothing (left) to drain this returns
        // on its first lock, so calling it on a spec-only wake costs one mutex round.
        io_drain(lane, seen);
        // Then use spare capacity on queued speculative reads, yielding the moment a real batch
        // arrives (drain_spec bails when batch_gen_ advances past this worker's `seen`).
        drain_spec(lane, seen);
    }
}

// Prefetch: on the eval thread, commit pages and enqueue speculative per-projection reads for the
// given experts of layer il. LRU-safe (same thread as load_layer); workers only read the bytes.
void ExpertStreamSource::prefetch(int il, const int32_t * ids, int n_ids) {
    if (!active_ || cache_max_ == 0 || il < 0 || il >= n_layer_ || !layers_[il].bound || !ids || n_ids <= 0) return;
    const LayerExperts & L = layers_[il];
    bool any = false;

    // Stage each expert's jobs locally and publish them only once ALL of its projections have been
    // committed. Pushing them as they were built meant a commit failure mid-expert could leave jobs
    // queued for an entry whose spec_remaining_ was never set: a worker would then decrement it from
    // zero, the entry could never reach zero to complete, the quiesce would never see it in
    // spec_touched_ to release, and the `!= 0` guard above would skip that expert's speculation for
    // the rest of the run. One failure, permanent damage — in exactly the low-memory situation this
    // path exists to degrade gracefully in.
    //
    // Committing before taking io_mtx_ matters for the same call: prefetch runs on the eval thread
    // right after a real batch was published, so holding the mutex across a syscall per projection
    // stalls the lanes trying to pull real read indices out of it. The pages are the eval thread's
    // to commit — no worker touches them until the jobs below exist.
    std::vector<IoJob> & staged = spec_stage_;
    std::vector<int32_t> & staged_ids = spec_stage_ids_;
    std::vector<int> & staged_counts = spec_stage_counts_;
    staged.clear();
    staged_ids.clear();
    staged_counts.clear();

    // Filter under ONE lock, not one per expert. This runs on the eval thread while every I/O lane
    // is taking the same mutex to pull its next read index, and a blocked eval thread is a
    // descheduled compute thread: measured on a phone, the per-expert acquisition was part of ~120
    // acquisitions per token that cost ~600 ms of wall (the same run with a single lane, hence no
    // contention, spent a quarter of the compute time on identical work).
    std::vector<int32_t> & cand = spec_cand_;
    cand.clear();
    {
        std::lock_guard<std::mutex> lk(io_mtx_);
        for (int i = 0; i < n_ids; ++i) {
            const int e = ids[i];
            if (e < 0 || e >= n_expert_) continue;
            const int32_t id = il * n_expert_ + e;
            if (cvalid_[id] || spec_remaining_[id] != 0) continue; // already resident or already queued
            cand.push_back(e);
        }
    }
    for (int e : cand) {
        const int32_t id = il * n_expert_ + e;
        const size_t mark = staged.size();
        int njobs = 0;
        bool ok = true;
        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            const uint64_t slice = L.proj[p].nb2;
            if (slice == 0) continue;
            char * dst = (char *) lbuf_[p][il] + (uint64_t) e * slice;
            uintptr_t a0 = (uintptr_t) dst & ~(uintptr_t) (page_ - 1);
            uintptr_t a1 = ((uintptr_t) dst + slice + page_ - 1) & ~(uintptr_t) (page_ - 1);
            if (!pio::vm_commit((void *) a0, (size_t) (a1 - a0))) { // low on memory — stop quietly
                ok = false;
                break;
            }
            staged.push_back({dst, L.proj[p].file_off + (uint64_t) e * slice, slice, id, (int16_t) L.proj[p].file_idx,
                              e, (int16_t) il, (int8_t) p, 1});
            ++njobs;
        }
        if (!ok) {
            // Hand back what this expert already committed and drop its jobs: an entry is queued
            // whole or not at all.
            staged.resize(mark);
            release_entry_pages(id);
            break;
        }
        if (njobs == 0) continue;
        if (ever_evicted_[id]) ++rereads_; // same accounting on the speculative path
        staged_ids.push_back(id);
        staged_counts.push_back(njobs);
        any = true;
    }
    if (!any) return;

    std::lock_guard<std::mutex> lk(io_mtx_);
    for (size_t s = 0; s < staged_ids.size(); ++s) {
        spec_remaining_[staged_ids[s]] = staged_counts[s];
        spec_touched_.push_back(staged_ids[s]);
    }
    spec_jobs_.insert(spec_jobs_.end(), staged.begin(), staged.end());
    if (prefetch_sync_) {
        // Test path: read the queued slices now on lane 0 (free on the eval thread in serial mode),
        // so the next quiesce integrates them deterministically. Mirrors drain_spec's accounting.
        for (;;) {
            IoJob j;
            uint64_t g;
            if (spec_next_ >= spec_jobs_.size()) break;
            g = spec_gen_;
            j = spec_jobs_[spec_next_++];
            ++spec_inflight_;
            const bool ok = read_slice(0, j);
            if (ok && g == spec_gen_) {
                spec_read_bytes_.fetch_add((long long) j.nbytes);
                if (--spec_remaining_[j.flag] == 0) {
                    spec_done_.push_back(j.flag);
                    spec_done_pending_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            --spec_inflight_;
        }
        return;
    }
    io_cv_.notify_all();
}

// Drain queued speculative reads on an idle lane. Bails as soon as a real batch this worker has
// not served appears, so speculation never delays real work. A completed entry (all projections
// read under the current spec generation) is handed to the eval thread via spec_done_.
void ExpertStreamSource::drain_spec(int lane, uint64_t worker_seen) {
    for (;;) {
        IoJob j;
        uint64_t g;
        {
            std::lock_guard<std::mutex> lk(io_mtx_);
            if (io_stop_ || batch_gen_ > worker_seen) return; // shutting down, or real work waiting
            if (spec_next_ >= spec_jobs_.size()) return;
            g = spec_gen_;
            j = spec_jobs_[spec_next_++];
            ++spec_inflight_;
        }
        const bool ok = read_slice(lane, j);
        {
            std::lock_guard<std::mutex> lk(io_mtx_);
            if (ok && g == spec_gen_) { // ignore reads from a cancelled round
                spec_read_bytes_.fetch_add((long long) j.nbytes);
                if (spec_remaining_[j.flag] > 0 && --spec_remaining_[j.flag] == 0) {
                    spec_done_.push_back(j.flag);
                    spec_done_pending_.fetch_add(1, std::memory_order_relaxed);
                    // The adoption wait watches per-entry counters, not just the in-flight count.
                    io_cv_done_.notify_all();
                }
            } else if (!ok && spec_adopt_) {
                // Forget-on-failure: zeroing the counter keeps the adoption wait from parking
                // forever, and !cvalid means a later demand read re-buys the entry.
                spec_remaining_[j.flag] = 0;
                io_cv_done_.notify_all();
            }
            if (--spec_inflight_ == 0) io_cv_done_.notify_all();
        }
    }
}

// Quiesce speculation before real staging: cancel queued reads, wait out in-flight ones, then on
// this (eval) thread integrate every fully-read entry into the cache and release the rest. All LRU
// mutation happens here, single-threaded, so it never races the real staging that follows.
void ExpertStreamSource::quiesce_spec(int adopt_il) {
    // Adoption (route-ahead only): the layer about to stage COMMITTED to the ids its speculation
    // was issued for, so its queued spec reads are not bets to discard — they are the layer's own
    // demand reads, already staged and possibly partly done. Keep exactly those, drop the rest of
    // the queue, and FINISH them here (this thread drains alongside any idle lane) before the
    // normal quiesce below integrates them as resident entries — which the staging then sees as
    // hits, so no byte is read twice. Without this, a committed layer's in-flight speculation was
    // released at its own load and re-read on demand: measured on the host at depth 2, every
    // early read was wasted that way (0% useful, double flash per token). The generation is NOT
    // bumped before the drain — bumping is what disowns reads, and these are being adopted.
    // Adoption (route-ahead only): under a committed routing, EVERY job in the spec queue is a
    // read some layer will demand verbatim — the loading layer's own jobs are its demand reads
    // already staged, and the other layers' jobs are demand reads a few callbacks early. So
    // nothing is cancelled: the loading layer's jobs move to the front and are finished now,
    // completed entries integrate, and everything else stays queued for the idle lanes. The
    // destructive quiesce below (cancel, disown, release) remains the right treatment for the
    // guessing predictors, whose queue really is bets. Measured before this branch existed: at
    // depth 2 every early read was destroyed — by the intervening load's quiesce or by the
    // settle preceding each issue — for 0% useful and double flash per token.
    if (spec_adopt_ && adopt_il >= 0) {
        std::vector<int32_t> & adopt = spec_adopt_ids_;
        adopt.clear();
        {
            std::lock_guard<std::mutex> lk(io_mtx_);
            std::stable_partition(spec_jobs_.begin() + (ptrdiff_t) spec_next_, spec_jobs_.end(),
                                  [&](const IoJob & j) { return j.layer == (int16_t) adopt_il; });
            for (int32_t id : spec_touched_)
                if (id / n_expert_ == adopt_il && spec_remaining_[id] != 0) adopt.push_back(id);
            // Without the full quiesce ever running, spec_touched_ would grow for the whole
            // session; entries fully settled need no release sweep, so drop them here.
            spec_touched_.erase(std::remove_if(spec_touched_.begin(), spec_touched_.end(),
                                               [&](int32_t id) { return spec_remaining_[id] == 0; }),
                                spec_touched_.end());
        }
        if (!adopt.empty()) {
            io_cv_.notify_all(); // idle worker lanes take the queue
            // Serially, lane 0 belongs to this thread, so it reads its own layer's jobs alongside
            // the workers; under overlap every lane is a worker and the eval thread must not
            // touch a reader — doing so raced worker 0 on one reader and fed the matmul
            // corrupted slices (first symptom: the routing agreeing BETTER with its own
            // prediction, garbage states drifting toward whatever the perturbed layers produce).
            //
            // Then WAIT for the loading layer's own entries, in both modes — measured, not
            // assumed: a no-wait variant that skipped these entries at staging and published
            // per-expert readiness from the completing reads was built and benchmarked, and it
            // LOST (5.8 vs 6.6 tok/s on the host at depth 2). Late integration enters the
            // entries cold, the eviction churns them, and the per-expert stalls it trades this
            // wait for cost more than the wait — which is short by construction, because these
            // jobs started a layer or more ago.
            if (!overlap_) drain_adopted(adopt_il);
            const auto tw0 = clock_t_::now();
            {
                std::unique_lock<std::mutex> lk(io_mtx_);
                io_cv_done_.wait(lk, [&] {
                    if (io_stop_) return true;
                    for (int32_t id : adopt)
                        if (spec_remaining_[id] != 0) return false;
                    return true;
                });
            }
            adopt_wait_ns_ +=
                (long long) std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - tw0).count();
        }
        spec_integrate_done();
        return;
    }
    std::vector<int32_t> done, touched;
    {
        std::unique_lock<std::mutex> lk(io_mtx_);
        ++spec_gen_; // cancel queued-but-unstarted reads and disown in-flight ones on completion
        spec_jobs_.clear();
        spec_next_ = 0;
        io_cv_done_.wait(lk, [&] { return spec_inflight_ == 0 || io_stop_; });
        done.swap(spec_done_);
        spec_done_pending_.store(0, std::memory_order_relaxed);
        touched.swap(spec_touched_);
    }
    // Integrate completed entries (all projections resident) into the LRU cache.
    for (int32_t id : done) {
        if (spec_remaining_[id] != 0 || cvalid_[id]) { // incomplete, or a real read already took it
            spec_remaining_[id] = 0;
            continue;
        }
        cvalid_[id] = 1;
        cspec_[id] = 1;  // speculative until a real lookup hits it (then counted useful)
        cstamp_[id] = 0; // not used this generation → evictable if the budget is tight
        cresident_ += entry_bytes(id / n_expert_);
        lru_push_back(id); // cold end: a mispredicted expert is reclaimed before any demanded one
        spec_experts_.fetch_add(1);
    }
    // Release pages of entries that never finished (a cancelled or failed read).
    for (int32_t id : touched) {
        if (spec_remaining_[id] == 0) continue; // completed above (or already integrated)
        release_entry_pages(id);
        spec_remaining_[id] = 0;
    }
}

// A token's pass over the layer stack is monotonic in il, so a non-increasing layer index means the
// previous token's pass just ended and the bytes it demanded are known. Prefill measures the
// batch's union (larger); the first decode token overwrites it with the decode value.
void ExpertStreamSource::account_demand(int il, int n_unique) {
    if (il <= last_il_) {
        token_demand_ = demand_accum_;
        layer_demand_ = layer_demand_accum_;
        demand_accum_ = 0;
        layer_demand_accum_ = 0;
    }
    last_il_ = il;
    const size_t bytes = (size_t) n_unique * entry_bytes(il);
    demand_accum_ += bytes;
    layer_demand_accum_ = std::max(layer_demand_accum_, bytes); // the widest layer of this pass
}

// Diagnostic telemetry (dense_resident_frac): how much of the dense set the kernel still has in RAM —
// the anon buffers under --dense-weights anon, the mmap ranges otherwise. This is the direct signal
// for whether a reclaim is dropping the model's own weights (and, under anon, whether zram is holding
// them). It feeds nothing (the governor is gone); it is measured only to be read. Throttled and timed
// into mgmt_ns_ so its cost is visible, not hidden in the compute residual. Eval-thread only.
void ExpertStreamSource::maybe_sample_dense() {
    if (++dense_probe_tick_ % dense_probe_every != 0) return;
    const auto t0 = clock_t_::now();
    dense_.sample_residency(page_);
    mgmt_ns_.fetch_add((long long) std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - t0).count());
}

// Explicit budget change from outside a decode (an app's memory-pressure callback, or the shrink
// gate). The only thing that moves the budget after init.
void ExpertStreamSource::set_cache_budget(size_t bytes) {
    if (cache_max_ == 0) return; // initialised off (shared-slot mode); the LRU buffers do not exist
    if (bytes > cache_max_) {
        // Growing evicts nothing, so it needs no quiesce — and must not do one: cancelling the
        // speculative reads in flight on every grow would quietly defeat --prefetch.
        cache_max_ = std::min(bytes, total_expert_bytes_);
        ++cache_resizes_;
        return;
    }
    if (bytes == cache_max_) return;
    quiesce_spec(); // cancel/drain spec reads so no worker is mid-write to an evicted page
    // Keep the budget strictly positive. The shared-slot buffers were never allocated (LRU mode was
    // chosen at init), and load_layer branches on cache_max_ == 0 to pick shared-slot vs LRU — so a
    // runtime zero would route into buffers that do not exist. This shrinks toward, never to, zero.
    cache_max_ = std::max<size_t>(1, bytes);
    ++cache_resizes_;
    // No cstamp guard: with no decode in flight, nothing is staged for the current generation, so
    // every resident entry (coldest first) is a valid eviction target.
    while (cresident_ > cache_max_ && ctail_ != -1)
        evict_tail();
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
// Insert at the LRU (cold) end. Speculative entries enter here so a wrong prediction is the first
// thing evicted and can never displace a demanded expert; a real lookup promotes it to the front.
void ExpertStreamSource::lru_push_back(int32_t id) {
    cnext_[id] = -1;
    cprev_[id] = ctail_;
    if (ctail_ != -1)
        cnext_[ctail_] = id;
    else
        chead_ = id;
    ctail_ = id;
}
size_t ExpertStreamSource::entry_bytes(int il) const {
    const LayerExperts & L = layers_[il];
    size_t bytes = 0;
    for (int p = 0; p < MoeRecipe::max_exps; ++p)
        bytes += (size_t) L.proj[p].nb2; // absent slots contribute 0
    return bytes;
}
// Release the physical pages fully contained in an entry's slices (never a partial page shared
// with a neighbouring expert). Shared by eviction and by discarding an incomplete spec entry.
void ExpertStreamSource::release_entry_pages(int32_t id) {
    const int il = id / n_expert_, e = id % n_expert_;
    for (int p = 0; p < MoeRecipe::max_exps; ++p) {
        const uint64_t slice = layers_[il].proj[p].nb2;
        if (slice == 0) continue; // absent slot in a fused layout
        char * s = (char *) lbuf_[p][il] + (uint64_t) e * slice;
        uintptr_t a0 = ((uintptr_t) s + page_ - 1) & ~(uintptr_t) (page_ - 1);
        uintptr_t a1 = ((uintptr_t) s + slice) & ~(uintptr_t) (page_ - 1);
        if (a1 > a0) pio::vm_evict((void *) a0, (size_t) (a1 - a0));
    }
}
void ExpertStreamSource::evict_tail() {
    const int32_t id = ctail_;
    ++evictions_;
    ever_evicted_[id] = 1; // the next read of this entry is the cache paying twice
    release_entry_pages(id);
    cvalid_[id] = 0;
    cspec_[id] = 0;
    cresident_ -= entry_bytes(id / n_expert_);
    lru_unlink(id);
}

// ── route-trace support: describe what a routing costs, without changing it ─────────
// Move already-resident predicted entries to the MRU end so eviction takes something else first.
// Deliberately NOT a cache hit: chits_ counts lookups the routing actually served, and a
// prediction is not a routing — inflating the hit rate from here would corrupt the one metric
// every cache decision in this project is argued from. Eval-thread only (LRU mutation).
void ExpertStreamSource::retain(int il, const int32_t * ids, int n_ids) {
    if (!active_ || cache_max_ == 0 || il < 0 || il >= n_layer_ || !layers_[il].bound || !ids) return;
    for (int i = 0; i < n_ids; ++i) {
        const int e = ids[i];
        if (e < 0 || e >= n_expert_) continue;
        const int32_t id = il * n_expert_ + e;
        if (!cvalid_[id]) continue; // absent: keeping it is prefetch's job, not retention's
        lru_unlink(id);
        lru_push_front(id);
    }
}

void ExpertStreamSource::settle_spec() {
    // Only the LRU path speculates, so mirror load_layer's own guard. Running the quiesce here
    // makes the one inside the load_layer that follows a cheap no-op: nothing left queued, and
    // nothing in flight. The cost is that its time lands outside the mgmt_ns_ window — which is
    // why a traced run is not a benchmark run.
    //
    // Under route-ahead the destructive settle would be a saboteur, not a bookkeeper: the hook
    // settles right before every issue, so the full quiesce here cancelled the future layers'
    // committed reads one callback before their load could adopt them. Integrate-only instead.
    if (!active_ || cache_max_ == 0) return;
    if (spec_adopt_) {
        // Cheap exit without touching the lanes' mutex: this runs before every route-ahead issue
        // (40 times a token) and usually has nothing to integrate. spec_done_pending_ is the
        // publisher's own count, so a relaxed read either sees work — and we take the lock — or
        // races a completion we will pick up at the next issue microseconds later.
        if (spec_done_pending_.load(std::memory_order_relaxed) != 0) spec_integrate_done();
    } else {
        quiesce_spec();
    }
}

// Integrate every completed speculative entry into the LRU cache, cancelling nothing — the
// route-ahead settle. Eval-thread only (LRU mutation), like the quiesce that subsumes it.
void ExpertStreamSource::spec_integrate_done() {
    std::vector<int32_t> & done = spec_done_scratch_;
    done.clear();
    {
        // BOTH eligibility guards run under io_mtx_, exactly as the destructive integrate runs
        // its own, and the remaining check is load-bearing: a completion can land in spec_done_
        // concurrently with (and just before) an issue that re-stages the same entry, and
        // integrating that stale completion would push an entry with LIVE queued jobs into the
        // LRU — the eviction then decommits pages a lane is about to write, which was this
        // feature's one segfault. The stale id is simply dropped; the new round's own completion
        // re-pushes it. After this snapshot the check cannot rot: re-staging happens only on
        // this same (eval) thread.
        std::lock_guard<std::mutex> lk(io_mtx_);
        for (int32_t id : spec_done_)
            if (!cvalid_[id] && spec_remaining_[id] == 0) done.push_back(id);
        spec_done_.clear();
        spec_done_pending_.store(0, std::memory_order_relaxed);
    }
    for (int32_t id : done) {
        cvalid_[id] = 1;
        cspec_[id] = 1;  // speculative until a real lookup hits it (then counted useful)
        cstamp_[id] = 0; // not used this generation → evictable if the budget is tight
        cresident_ += entry_bytes(id / n_expert_);
        // Cold end for a GUESS (a mispredicted expert should be the first thing reclaimed) — but
        // under route-ahead there are no guesses: every integrated entry is a read some layer
        // COMMITTED to, at most N layers from being routed. Entering it cold handed it to the
        // very evictions of the intervening layers' loads, and on a device whose cache runs at
        // capacity that destroyed the entries before use and re-bought them on demand — a
        // perfect prefetch paying its bytes twice (measured: +34% read/token, drop off). A
        // committed read enters HOT; its own routing arrives to promote it within N layers
        // anyway.
        if (spec_adopt_)
            lru_push_front(id);
        else
            lru_push_back(id);
        spec_experts_.fetch_add(1);
    }
}

// Serial-mode helper for adoption: this thread owns lane 0, so it reads the loading layer's own
// adopted jobs — which sit at the front after the partition — and stops at the first job that
// belongs to a future layer, which stays queued for the worker lanes' idle time.
void ExpertStreamSource::drain_adopted(int adopt_il) {
    for (;;) {
        IoJob j;
        {
            std::lock_guard<std::mutex> lk(io_mtx_);
            if (spec_next_ >= spec_jobs_.size() || spec_jobs_[spec_next_].layer != (int16_t) adopt_il) return;
            j = spec_jobs_[spec_next_++];
            ++spec_inflight_;
        }
        const bool ok = read_slice(0, j);
        {
            std::lock_guard<std::mutex> lk(io_mtx_);
            if (ok) {
                spec_read_bytes_.fetch_add((long long) j.nbytes);
                if (spec_remaining_[j.flag] > 0 && --spec_remaining_[j.flag] == 0) {
                    spec_done_.push_back(j.flag);
                    spec_done_pending_.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                // A failed read forgets the entry: zeroing its counter keeps the adoption wait
                // from hanging, and !cvalid means the demand path simply re-reads it.
                spec_remaining_[j.flag] = 0;
            }
            if (--spec_inflight_ == 0) io_cv_done_.notify_all();
        }
    }
}

void ExpertStreamSource::query_residency(int il, const int32_t * ids, int n_ids, uint8_t * out) const {
    for (int i = 0; i < n_ids; ++i)
        out[i] = 0;
    if (!active_ || il < 0 || il >= n_layer_ || !layers_[il].bound || !ids) return;
    if (cache_max_ == 0) return; // shared-slot mode: nothing is kept, so every routing re-reads
    for (int i = 0; i < n_ids; ++i) {
        const int e = ids[i];
        if (e < 0 || e >= n_expert_) continue;
        const int32_t id = il * n_expert_ + e;
        if (!cvalid_[id]) continue;  // miss: this routing pays a read
        out[i] = cspec_[id] ? 2 : 1; // resident; 2 = a speculative prefetch guessed it right
    }
}

uint64_t ExpertStreamSource::expert_bytes(int il) const {
    if (!active_ || il < 0 || il >= n_layer_ || !layers_[il].bound) return 0;
    return (uint64_t) entry_bytes(il);
}

// The cache accounting both load paths share. See the header for why this is the part that is shared
// and the job emission is not. Eval-thread only, like every other LRU mutation.
bool ExpertStreamSource::commit_proj_pages(int il, int e, int p) {
    const LayerExperts & L = layers_[il];
    const uint64_t slice = L.proj[p].nb2;
    if (slice == 0) return true; // absent slot in a fused layout
    char * dst = (char *) lbuf_[p][il] + (uint64_t) e * slice;
    uintptr_t a0 = (uintptr_t) dst & ~(uintptr_t) (page_ - 1);
    uintptr_t a1 = ((uintptr_t) dst + slice + page_ - 1) & ~(uintptr_t) (page_ - 1);
    if (!pio::vm_commit((void *) a0, (size_t) (a1 - a0))) {
        std::fprintf(stderr, "bmoe: commit failed\n");
        return false;
    }
    return true;
}

bool ExpertStreamSource::touch_entry(int il, int e, bool & hit, bool promote, int commit_only_proj) {
    const int32_t id = il * n_expert_ + e;
    clookups_++;
    cstamp_[id] = cgen_;
    if (cvalid_[id]) {
        chits_++;
        if (cspec_[id]) { // this hit was served by a speculative prefetch — count it useful once
            spec_useful_.fetch_add(1);
            cspec_[id] = 0;
        }
        if (promote) {
            lru_unlink(id);
            lru_push_front(id);
        }
        hit = true;
        return true;
    }
    // Miss: commit the pages the caller's reads will fill, then enter the expert as resident. The
    // entry is valid from here on because the caller is contracted to schedule those reads before
    // anything can consume the slices — the barrier that makes this safe is the eval-callback's.
    // (With commit_only_proj, the caller also contracts to commit the other projections before it
    // emits their jobs; the residency accounting below still covers the whole entry, because the
    // entry lives and is evicted as a unit either way.)
    for (int p = 0; p < MoeRecipe::max_exps; ++p) {
        if (commit_only_proj >= 0 && p != commit_only_proj) continue;
        if (!commit_proj_pages(il, e, p)) return false;
    }
    if (ever_evicted_[id]) ++rereads_; // this entry was resident once; the cache is buying it again
    cvalid_[id] = 1;
    cspec_[id] = 0; // a real read, not speculative
    cresident_ += entry_bytes(il);
    lru_push_front(id);
    hit = false;
    return true;
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

    // Integrate / discard any speculative prefetch before this layer's real staging touches the
    // cache. After this returns no spec read is in flight and completed ones are resident hits. The
    // budget is fixed for the run (auto sizes it once at init), so there is nothing to resize here.
    if (cache_max_) quiesce_spec(spec_adopt_ ? il : -1);

    auto stage = [&](int e) -> bool {
        if (cache_max_ == 0) {
            for (int p = 0; p < MoeRecipe::max_exps; ++p) {
                const uint64_t slice = L.proj[p].nb2;
                if (slice == 0) continue; // absent slot in a fused layout
                jobs_.push_back({(char *) slot_[p] + (uint64_t) e * slice, L.proj[p].file_off + (uint64_t) e * slice,
                                 slice, -1, (int16_t) L.proj[p].file_idx, e, (int16_t) il, (int8_t) p, 0});
            }
            return true;
        }
        bool hit = false;
        if (!touch_entry(il, e, hit)) return false;
        if (hit) return true;
        // A miss: emit this expert's reads, expert-major. The drain below waits on them all, so the
        // order only has to be complete, not clever — unlike the overlap path, where it feeds a
        // kernel that is already blocking.
        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            const uint64_t slice = L.proj[p].nb2;
            if (slice == 0) continue; // absent slot in a fused layout
            jobs_.push_back({(char *) lbuf_[p][il] + (uint64_t) e * slice, L.proj[p].file_off + (uint64_t) e * slice,
                             slice, -1, (int16_t) L.proj[p].file_idx, e, (int16_t) il, (int8_t) p, 0});
        }
        return true;
    };

    std::fill(seen_.begin(), seen_.end(), (uint8_t) 0);
    int n_unique = 0;
    for (int i = 0; i < n_ids; ++i) {
        const int e = load_all_ ? (i < n_expert_ ? i : -1) : ids[i];
        if (e < 0 || e >= n_expert_) continue;
        if (seen_[e]) {
            // Already staged in this batch: still promote so the LRU order reflects the LAST
            // token that used this expert (ids arrive token-major), not its first touch — this
            // keeps the prompt tail's experts hot across prefill. Reads are scheduled only once
            // (the seen_ guard below), so this is bookkeeping only. The router's own top-k ids are
            // distinct, so in decode (n=1) this used to be unreachable — but cache-aware dropping
            // repoints a dropped slot's id at the routing's top expert, which makes duplicates the
            // normal case there. Promoting the same entry twice is idempotent, so it stays correct.
            if (cache_max_) {
                const int32_t id = il * n_expert_ + e;
                lru_unlink(id);
                lru_push_front(id);
            }
            continue;
        }
        seen_[e] = 1;
        ++n_unique;
        if (!stage(e)) return false;
    }
    if (load_all_) {
        for (int e = 0; e < n_expert_; ++e) {
            if (seen_[e]) continue;
            seen_[e] = 1;
            ++n_unique;
            if (!stage(e)) return false;
        }
    }
    account_demand(il, n_unique);
    maybe_sample_dense();

    const size_t njobs = jobs_.size();
    mgmt_ns_.fetch_add((long long) std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - tm0).count());
    const auto t0 = clock_t_::now();
    if (io_threads_ <= 1 || njobs <= 1) {
        for (size_t i = 0; i < njobs; ++i) {
            const IoJob & j = jobs_[i];
            if (!read_slice(0, j)) return false;
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
        const auto tw0 = clock_t_::now();
        std::unique_lock<std::mutex> lk(io_mtx_);
        io_cv_done_.wait(lk, [&] { return done_cnt_ == batch_njobs_ || io_stop_; });
        drain_wait_ns_ +=
            (long long) std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - tw0).count();
    }
    if (fatal_.load(std::memory_order_acquire)) return false;

    // Cache-management work below (dedup/sort, vm commit, LRU bookkeeping and the eviction at
    // the end) runs on the eval thread and would otherwise hide inside the compute residual. Time
    // it into mgmt_ns_ so the metrics can surface it. The drain wait above is I/O, not mgmt.
    const auto tm0 = clock_t_::now();

    // Integrate / discard speculative prefetch before this layer's real staging (the previous
    // batch is already drained above, so this only waits on in-flight spec reads). The budget is
    // fixed for the run, so there is nothing to resize here.
    if (cache_max_) quiesce_spec(spec_adopt_ ? il : -1);

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
    account_demand(il, (int) staged_.size());
    maybe_sample_dense();

    bool published = false; // a two-wave batch publishes inside the staging branch

    if (cache_max_ == 0) {
        // Cache off: every (projection, expert) is a fresh read into the shared full-size slot
        // at its canonical offset e*slice. Emit projection-major.
        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            const uint64_t slice = L.proj[p].nb2;
            if (slice == 0) continue; // absent slot in a fused layout
            for (int e : staged_) {
                const int32_t flag = (int32_t) ((size_t) p * (size_t) n_expert_ + (size_t) e);
                jobs_.push_back({(char *) slot_[p] + (uint64_t) e * slice, L.proj[p].file_off + (uint64_t) e * slice,
                                 slice, flag, (int16_t) L.proj[p].file_idx, e, (int16_t) il, (int8_t) p, 0});
            }
        }
    } else {
        // Cache on: per-expert LRU bookkeeping (hit → mark all projections ready now; miss →
        // commit the pages and remember it). Then emit the misses' jobs projection-major.
        //
        // Two-wave publish (#118): normally no lane can start reading until every miss took up
        // to three page-commit syscalls — bookkeeping sitting in front of the first byte of I/O,
        // on the latency-to-first-slice path. With two_wave_, only the FIRST present projection
        // (the one mul_mat_id blocks on first) is committed up front; its jobs publish
        // immediately, and the remaining projections are committed and appended while the lanes
        // already read. io_drain copies jobs under the lock and the worker wait predicate admits
        // a grown batch, so appending mid-generation is safe.
        const int p0 = [&] {
            for (int p = 0; p < MoeRecipe::max_exps; ++p)
                if (L.proj[p].nb2) return p;
            return -1;
        }();
        const bool two_wave = two_wave_ && !load_all_ && p0 >= 0;
        seen_.assign(seen_.size(), 0); // reuse as a per-staged miss marker keyed by expert
        for (int e : staged_) {
            bool hit = false;
            // promote=false: the token-major loop below re-orders every touched id anyway, so a
            // hit-path LRU move here was k wasted pointer operations per layer per token.
            if (!touch_entry(il, e, hit, /*promote=*/false, two_wave ? p0 : -1)) {
                // Nothing waits on a flag we will never publish: abort the graph instead.
                fatal_.store(true, std::memory_order_release);
                return false;
            }
            if (hit) { // resolved without a read — release every projection's waiter now
                for (int p = 0; p < MoeRecipe::max_exps; ++p)
                    if (L.proj[p].nb2) mark_ready(p, e);
                continue;
            }
            seen_[e] = 1; // this expert missed → its projections need reads
        }
        auto emit_proj = [&](int p) {
            const uint64_t slice = L.proj[p].nb2;
            if (slice == 0) return;
            for (int e : staged_) {
                if (!seen_[e]) continue; // cache hit, already marked ready
                const int32_t flag = (int32_t) ((size_t) p * (size_t) n_expert_ + (size_t) e);
                jobs_.push_back({(char *) lbuf_[p][il] + (uint64_t) e * slice,
                                 L.proj[p].file_off + (uint64_t) e * slice, slice, flag, (int16_t) L.proj[p].file_idx,
                                 e, (int16_t) il, (int8_t) p, 0});
            }
        };
        if (!two_wave) {
            for (int p = 0; p < MoeRecipe::max_exps; ++p)
                emit_proj(p);
        } else {
            // Wave one: the first projection's jobs, published before anything else is committed.
            emit_proj(p0);
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
            published = true;
            // Wave two: commit the remaining projections' pages, then append their jobs. A commit
            // failure here is after wave one published, so waiters may already be blocked on
            // flags this batch will now never flip — go fatal and wake them to observe it.
            for (int e : staged_) {
                if (!seen_[e]) continue;
                for (int p = 0; p < MoeRecipe::max_exps; ++p) {
                    if (p == p0) continue;
                    if (!commit_proj_pages(il, e, p)) {
                        fatal_.store(true, std::memory_order_release);
                        {
                            std::lock_guard<std::mutex> lk(ready_mtx_);
                            ready_cv_.notify_all();
                        }
                        return false;
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lk(io_mtx_);
                for (int p = 0; p < MoeRecipe::max_exps; ++p)
                    if (p != p0) emit_proj(p);
                batch_njobs_ = jobs_.size();
            }
            io_cv_.notify_all();
        }

        // Promote every touched expert in raw id order (token-major) so the LRU order reflects
        // the LAST token that used it, not the sorted/first-touch order staged_ imposes — this
        // keeps the prompt tail's experts hot across prefill. This loop is the ONLY promotion on
        // this path (touch_entry above runs with promote=false).
        // Skipped in load_all (everything is resident, so LRU order is meaningless).
        //
        // The cvalid guard is load-bearing, not defensive: an ADOPTED-PENDING expert (route-ahead)
        // was deliberately never staged, so it is not in the LRU — and lru_unlink on a node whose
        // prev/next are both -1 rewrites chead_/ctail_ to -1, orphaning every linked entry in one
        // call. That was this feature's second segfault: the next push_front made the pending
        // entry the LIST, eviction took it (decommitting pages its reads were still filling), and
        // the orphaned entries could never be evicted again. A pending expert needs no promotion:
        // it enters the LRU at integration, and its first real hit promotes it then.
        if (!load_all_) {
            for (int i = 0; i < n_ids; ++i) {
                const int e = ids[i];
                if (e < 0 || e >= n_expert_) continue;
                const int32_t id = il * n_expert_ + e;
                if (!cvalid_[id]) continue;
                lru_unlink(id);
                lru_push_front(id);
            }
        }
    }

    // 4. Publish the batch and return immediately — no drain. Workers fill the slices and
    //    flip the flags as they go; the compute threads block on those flags via the hook.
    //    (A two-wave batch already published both waves inside the staging branch.)
    if (!published) {
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
    }

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

// One idle beat of the spin-wait below: keep the core out of the sibling threads' way without
// entering the scheduler. yield() is a syscall; these are single instructions.
static inline void cpu_relax() {
#if defined(__aarch64__)
    __asm__ __volatile__("isb" ::: "memory");
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
    _mm_pause();
#else
    __builtin_ia32_pause();
#endif
#else
    std::this_thread::yield();
#endif
}

// Called from EVERY compute thread, for each routed expert, before it reads that expert's rows.
// Blocks until the expert's slice for the layer in flight is resident (or the run goes fatal).
void ExpertStreamSource::on_expert_ready(const ggml_tensor * src0, int expert) {
    const void * key = (const void *) src0;
    auto it = std::lower_bound(texp_.begin(), texp_.end(), key,
                               [](const std::pair<const void *, uint32_t> & a, const void * k) { return a.first < k; });
    if (it == texp_.end() || it->first != key) return; // not one of our streamed expert tensors
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
    // Short spin first: a slice usually lands within microseconds, cheaper than a syscall. The
    // beat is a pause instruction, not yield() — 2048 yields burnt up to a millisecond of
    // sched_yield churn per genuinely slow slice, stealing CPU from the I/O lanes and the
    // sibling compute threads that would have finished the slice sooner.
    for (int s = 0; s < 256; ++s) {
        if (ready_[idx].gen.load(std::memory_order_acquire) == want || fatal_.load(std::memory_order_acquire)) {
            stall_ns_.fetch_add(
                (long long) std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - t0).count());
            return;
        }
        cpu_relax();
    }
    // Register as a waiter BEFORE the last look at the flag. The publisher sets the flag before it
    // reads this count, and both sides are seq_cst, so one of the two must observe the other: either
    // the flag is already set here and this thread never sleeps, or the publisher sees a non-zero
    // count and notifies. That is what lets io_drain skip the mutex and the notify_all when no
    // compute thread is blocked, which is the usual case.
    ready_waiters_.fetch_add(1, std::memory_order_seq_cst);
    if (ready_[idx].gen.load(std::memory_order_seq_cst) != want && !fatal_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk(ready_mtx_);
        ready_cv_.wait(lk, [&] {
            return ready_[idx].gen.load(std::memory_order_acquire) == want || fatal_.load(std::memory_order_acquire);
        });
    }
    ready_waiters_.fetch_sub(1, std::memory_order_seq_cst);
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
    long long rd_bytes = 0, rd_syscall_ns = 0;
    for (const auto & r : readers_) {
        rd_bytes += r->read_bytes();
        rd_syscall_ns += r->syscall_ns();
    }
    s.read_bytes = (uint64_t) rd_bytes;
    // Serial: read_ns_ is the wall time the caller was blocked in the read phase. Overlap: the
    // caller never blocks, so "I/O time" is instead the summed lane-busy time (the readers' syscall
    // ns), which the runtime reports as lane-busy per token rather than as a slice of wall time.
    s.read_seconds = (overlap_ ? rd_syscall_ns : read_ns_.load()) / 1e9;
    s.mgmt_seconds = mgmt_ns_.load() / 1e9;
    s.spec_read_bytes = (uint64_t) spec_read_bytes_.load();
    s.spec_experts = spec_experts_.load();
    s.spec_useful = spec_useful_.load();
    s.cache_hits = chits_;
    s.cache_lookups = clookups_;
    s.cache_resident_bytes = (uint64_t) cresident_;
    s.stall_seconds = stall_ns_.load() / 1e9;
    s.cache_budget_bytes = (uint64_t) cache_max_;
    s.cache_resizes = cache_resizes_;
    s.evictions = evictions_;
    s.rereads = rereads_;
    s.drain_wait_seconds = drain_wait_ns_ / 1e9;
    s.adopt_wait_seconds = adopt_wait_ns_ / 1e9;
    s.dense_resident_frac = dense_.resident_frac();
    s.token_demand_bytes = (uint64_t) token_demand_;
    s.layer_demand_bytes = (uint64_t) layer_demand_;
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
        ++spec_gen_; // disown any in-flight speculative reads
        spec_jobs_.clear();
        spec_next_ = 0;
    }
    io_cv_.notify_all();
    io_cv_done_.notify_all();
    for (auto & t : io_pool_)
        if (t.joinable()) t.join();
    io_pool_.clear();
    spec_done_.clear();
    spec_done_pending_.store(0, std::memory_order_relaxed);
    spec_touched_.clear();

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
    // The dense-weights module frees its own anon buffers here. Safe: the context (whose rebound
    // dense tensors point at them) is torn down after the source, and no decode is in flight — same
    // contract as the slot and LRU buffers freed above.
    dense_.shutdown();
    dense_tensors_.clear();
    readers_.clear(); // closes every shard's lane fds and frees the bounces
    jobs_.clear();
    layers_.clear();
    active_ = false;
}

} // namespace bmoe
