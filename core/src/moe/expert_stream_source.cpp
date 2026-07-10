#include "expert_stream_source.h"

#include "ggml.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace bmoe {

using clock_t_ = std::chrono::steady_clock;

ExpertStreamSource::~ExpertStreamSource() { shutdown(); }

// ── init: allocate buffers, rebind expert tensors, start the read pool ──────────────
bool ExpertStreamSource::init(const std::string & gguf_path, int n_expert,
                              std::vector<LayerExperts> layers, const MoeStreamConfig & cfg) {
    if (active_) return false;
    if (n_expert <= 0) {
        std::fprintf(stderr, "bmoe: expert streaming needs a MoE model (n_expert=%d)\n", n_expert);
        return false;
    }

    n_expert_  = n_expert;
    layers_    = std::move(layers);
    n_layer_   = (int) layers_.size();
    o_direct_  = cfg.o_direct;
    load_all_  = cfg.load_all;
    cache_max_ = (size_t) std::max(0, cfg.cache_mb) * 1024ull * 1024ull;
    io_threads_ = std::max(1, std::min(MoeStreamConfig::io_threads_max, cfg.io_threads));

    // Largest full-tensor byte size per projection, over all bound layers → shared-slot
    // and bounce sizing.
    size_t max_full[3] = {0, 0, 0};
    for (const LayerExperts & L : layers_) {
        if (!L.bound) continue;
        for (int p = 0; p < 3; ++p) {
            const size_t full = (size_t) L.proj[p].nb2 * (size_t) n_expert_;
            max_full[p] = std::max(max_full[p], full);
        }
    }
    if (max_full[0] == 0) {
        std::fprintf(stderr, "bmoe: no MoE layers were bound\n");
        return false;
    }

    if (cache_max_ == 0) {
        // Three shared slots reused across layers (one layer computes at a time). Rebind
        // every bound layer's expert tensors onto them; only routed slices are ever valid.
        for (int p = 0; p < 3; ++p) {
            slot_[p] = pio::aligned_alloc(align_, max_full[p]);
            if (!slot_[p]) { std::fprintf(stderr, "bmoe: slot alloc %zu failed\n", max_full[p]); return false; }
            slot_sz_[p] = max_full[p];
        }
        for (LayerExperts & L : layers_) {
            if (!L.bound) continue;
            for (int p = 0; p < 3; ++p) L.proj[p].tensor->data = slot_[p];
        }
    } else {
        // LRU cache: one reserved (address-only) buffer per (layer, projection). Physical
        // pages appear on the first miss and are released on eviction. mul_mat_id needs
        // each expert at its canonical offset e*nb2 inside tensor->data, so the buffers
        // live at fixed per-layer addresses; lazy commit keeps that affordable.
        page_ = pio::vm_page();
        for (int p = 0; p < 3; ++p) {
            lbuf_[p].assign(n_layer_, nullptr);
            lbuf_sz_[p].assign(n_layer_, 0);
        }
        for (int il = 0; il < n_layer_; ++il) {
            LayerExperts & L = layers_[il];
            if (!L.bound) continue;
            for (int p = 0; p < 3; ++p) {
                const size_t full = (size_t) L.proj[p].nb2 * (size_t) n_expert_;
                lbuf_[p][il] = pio::vm_reserve(full);
                if (!lbuf_[p][il]) { std::fprintf(stderr, "bmoe: vm_reserve %zu failed (layer %d)\n", full, il); return false; }
                lbuf_sz_[p][il]  = full;
                L.proj[p].tensor->data = lbuf_[p][il];
            }
        }
        const size_t n_entry = (size_t) n_layer_ * n_expert_;
        cvalid_.assign(n_entry, 0);
        cstamp_.assign(n_entry, 0);
        cprev_.assign(n_entry, -1);
        cnext_.assign(n_entry, -1);
        chead_ = ctail_ = -1;
        cresident_ = 0; cgen_ = 0; chits_ = 0; clookups_ = 0;
    }

    // Read pool: a private fd + bounce per lane so concurrent preads never contend.
    const size_t max_slice  = std::max({max_full[0], max_full[1], max_full[2]}) / (size_t) n_expert_;
    const size_t bounce_cap = max_slice + 2 * align_;

    pio::fd_t primary = pio::open_read(gguf_path.c_str(), o_direct_);
    if (!pio::fd_ok(primary) && o_direct_) { o_direct_ = false; primary = pio::open_read(gguf_path.c_str(), false); }
    if (!pio::fd_ok(primary)) { std::fprintf(stderr, "bmoe: open %s failed\n", gguf_path.c_str()); return false; }
    fsize_ = pio::file_size(primary);

    fds_.assign(io_threads_, pio::fd_invalid);
    fds_buf_.assign(io_threads_, pio::fd_invalid);
    bounces_.assign(io_threads_, nullptr);
    bounce_sz_.assign(io_threads_, 0);
    for (int lane = 0; lane < io_threads_; ++lane) {
        fds_[lane]     = (lane == 0) ? primary : pio::open_read(gguf_path.c_str(), o_direct_);
        fds_buf_[lane] = o_direct_ ? pio::open_read(gguf_path.c_str(), false) : pio::fd_invalid;
        if (!pio::fd_ok(fds_[lane])) { std::fprintf(stderr, "bmoe: lane %d open failed\n", lane); return false; }
        bounces_[lane] = pio::aligned_alloc(align_, bounce_cap);
        if (!bounces_[lane]) { std::fprintf(stderr, "bmoe: lane %d bounce alloc failed\n", lane); return false; }
        bounce_sz_[lane] = bounce_cap;
    }

    seen_.assign(n_expert_, 0);
    jobs_.reserve((size_t) n_expert_ * 3);
    batch_gen_ = 0; next_idx_ = 0; done_cnt_ = 0; io_stop_ = false; io_err_.store(false);

    active_ = true;
    for (int lane = 1; lane < io_threads_; ++lane) io_pool_.emplace_back(&ExpertStreamSource::io_worker, this, lane);

    std::fprintf(stderr, "bmoe: expert streaming ON  n_expert=%d o_direct=%d io_threads=%d cache=%zu MiB\n",
                 n_expert_, (int) o_direct_, io_threads_, cache_max_ >> 20);
    return true;
}

// ── one aligned slice read on a lane ────────────────────────────────────────────────
bool ExpertStreamSource::read_slice(int lane, void * dst, uint64_t off, uint64_t nbytes) {
    if (nbytes == 0) return true;
    const uint64_t a0  = off & ~(uint64_t) (align_ - 1);
    const uint64_t a1  = (off + nbytes + align_ - 1) & ~(uint64_t) (align_ - 1);
    const size_t   len = (size_t) (a1 - a0);
    if (bounce_sz_[lane] < len) {
        if (bounces_[lane]) pio::aligned_free(bounces_[lane]);
        bounces_[lane] = pio::aligned_alloc(align_, len);
        if (!bounces_[lane]) { std::fprintf(stderr, "bmoe: bounce realloc %zu failed\n", len); return false; }
        bounce_sz_[lane] = len;
    }
    char * b = (char *) bounces_[lane];
    const pio::fd_t fd     = fds_[lane];
    const pio::fd_t fd_buf = fds_buf_[lane];
    const uint64_t read_end = (fsize_ && a1 > fsize_) ? fsize_ : a1;
    const uint64_t bulk_end = o_direct_ ? (read_end & ~(uint64_t) (align_ - 1)) : read_end;

    const auto t0 = clock_t_::now();
    for (uint64_t a = a0; a < bulk_end;) {
        long long got = pio::pread_at(fd, b + (a - a0), (size_t) (bulk_end - a), a);
        if (got <= 0) { std::fprintf(stderr, "bmoe: pread failed at %llu\n", (unsigned long long) a); return false; }
        a += (uint64_t) got;
    }
    for (uint64_t a = bulk_end; a < read_end;) {   // sub-alignment EOF tail via the buffered fd
        long long got = pio::pread_at(pio::fd_ok(fd_buf) ? fd_buf : fd, b + (a - a0), (size_t) (read_end - a), a);
        if (got <= 0) { std::fprintf(stderr, "bmoe: tail pread failed at %llu\n", (unsigned long long) a); return false; }
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
        if (!read_slice(lane, j.dst, j.off, j.nbytes)) io_err_.store(true);
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
            g = batch_gen_; seen = g;
        }
        io_drain(lane, g);
    }
}

// ── LRU plumbing ────────────────────────────────────────────────────────────────────
void ExpertStreamSource::lru_unlink(int32_t id) {
    int32_t pv = cprev_[id], nx = cnext_[id];
    if (pv != -1) cnext_[pv] = nx; else chead_ = nx;
    if (nx != -1) cprev_[nx] = pv; else ctail_ = pv;
    cprev_[id] = cnext_[id] = -1;
}
void ExpertStreamSource::lru_push_front(int32_t id) {
    cprev_[id] = -1; cnext_[id] = chead_;
    if (chead_ != -1) cprev_[chead_] = id; else ctail_ = id;
    chead_ = id;
}
size_t ExpertStreamSource::entry_bytes(int il) const {
    const LayerExperts & L = layers_[il];
    return (size_t) L.proj[0].nb2 + L.proj[1].nb2 + L.proj[2].nb2;
}
void ExpertStreamSource::evict_tail() {
    const int32_t id = ctail_;
    const int il = id / n_expert_, e = id % n_expert_;
    for (int p = 0; p < 3; ++p) {
        const uint64_t slice = layers_[il].proj[p].nb2;
        char * s = (char *) lbuf_[p][il] + (uint64_t) e * slice;
        uintptr_t a0 = ((uintptr_t) s + page_ - 1) & ~(uintptr_t) (page_ - 1);
        uintptr_t a1 = ((uintptr_t) s + slice)     & ~(uintptr_t) (page_ - 1);
        if (a1 > a0) pio::vm_evict((void *) a0, (size_t) (a1 - a0));
    }
    cvalid_[id] = 0;
    cresident_ -= entry_bytes(il);
    lru_unlink(id);
}

// ── load: stage routed experts, read the batch, evict cold entries to budget ────────
bool ExpertStreamSource::load_layer(int il, const int32_t * ids, int n_ids) {
    if (!active_ || il < 0 || il >= n_layer_ || !layers_[il].bound || !ids || n_ids <= 0) return false;
    LayerExperts & L = layers_[il];
    cgen_++;
    jobs_.clear();

    auto stage = [&](int e) -> bool {
        if (cache_max_ == 0) {
            for (int p = 0; p < 3; ++p) {
                const uint64_t slice = L.proj[p].nb2;
                jobs_.push_back({(char *) slot_[p] + (uint64_t) e * slice, L.proj[p].file_off + (uint64_t) e * slice, slice});
            }
            return true;
        }
        const int32_t id = il * n_expert_ + e;
        clookups_++;
        cstamp_[id] = cgen_;
        if (cvalid_[id]) { chits_++; lru_unlink(id); lru_push_front(id); return true; }
        for (int p = 0; p < 3; ++p) {
            const uint64_t slice = L.proj[p].nb2;
            char * dst = (char *) lbuf_[p][il] + (uint64_t) e * slice;
            uintptr_t a0 = (uintptr_t) dst                          & ~(uintptr_t) (page_ - 1);
            uintptr_t a1 = ((uintptr_t) dst + slice + page_ - 1)    & ~(uintptr_t) (page_ - 1);
            if (!pio::vm_commit((void *) a0, (size_t) (a1 - a0))) { std::fprintf(stderr, "bmoe: commit failed\n"); return false; }
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
            batch_njobs_ = njobs; next_idx_ = 0; done_cnt_ = 0; io_err_.store(false);
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
        while (cresident_ > cache_max_ && ctail_ != -1 && cstamp_[ctail_] != cgen_) evict_tail();
    }
    return true;
}

IExpertSource::Stats ExpertStreamSource::stats() const {
    Stats s;
    s.read_bytes    = (uint64_t) read_bytes_.load();
    s.read_seconds  = read_ns_.load() / 1e9;
    s.cache_hits    = chits_;
    s.cache_lookups = clookups_;
    s.cache_resident_bytes = (uint64_t) cresident_;
    return s;
}

void ExpertStreamSource::shutdown() {
    if (!active_) return;
    { std::lock_guard<std::mutex> lk(io_mtx_); io_stop_ = true; }
    io_cv_.notify_all();
    io_cv_done_.notify_all();
    for (auto & t : io_pool_) if (t.joinable()) t.join();
    io_pool_.clear();

    for (int p = 0; p < 3; ++p) if (slot_[p]) { pio::aligned_free(slot_[p]); slot_[p] = nullptr; }
    for (int p = 0; p < 3; ++p) {
        for (int il = 0; il < (int) lbuf_[p].size(); ++il)
            if (lbuf_[p][il]) pio::vm_release(lbuf_[p][il], lbuf_sz_[p][il]);
        lbuf_[p].clear(); lbuf_sz_[p].clear();
    }
    for (void * b : bounces_) if (b) pio::aligned_free(b);
    bounces_.clear(); bounce_sz_.clear();
    for (int lane = 0; lane < (int) fds_.size(); ++lane) {
        if (pio::fd_ok(fds_[lane]))     pio::close_fd(fds_[lane]);
        if (pio::fd_ok(fds_buf_[lane])) pio::close_fd(fds_buf_[lane]);
    }
    fds_.clear(); fds_buf_.clear();
    jobs_.clear();
    layers_.clear();
    active_ = false;
}

} // namespace bmoe
