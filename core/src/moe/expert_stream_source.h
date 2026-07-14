// Flash-backed expert streaming with an optional LRU cache.
//
// Ported from the original research streamer. One layer computes at a time, so with the
// cache off the expert tensors share up to three heap slots (full n_expert size) that
// every layer's tensors are rebound onto: only the routed slices are ever filled,
// re-read fresh each token. With the cache on, each (layer, projection) gets its own
// reserved-but-uncommitted address range; a routed expert already resident is a hit (no
// read), a miss is read once and kept, and the coldest entries are evicted (pages
// physically released) to hold the budget. Reads are drained across an I/O lane pool.
//
// Correctness rests on the eval-callback single-node barrier: the routing node is
// computed and synchronized before load_layer() runs, and the layer's expert matmul
// runs only after it returns — so the routed slices are valid exactly when read, and the
// next layer cannot overwrite them until this layer's matmul has been synchronized.
#pragma once

#include "bmoe/expert_source.h"
#include "bmoe/config.h"
#include "bmoe/recipe.h"
#include "../io/platform_io.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct ggml_tensor;

namespace bmoe {

// One expert weight tensor to rebind, with where its data lives in the gguf.
struct ExpertTensorRef {
    ggml_tensor * tensor = nullptr; // persistent weight tensor whose ->data we rebind
    uint64_t file_off = 0;          // absolute file offset of the tensor's data
    uint64_t nb2 = 0;               // bytes per expert (== tensor->nb[2])
};

// The expert weight tensors of one MoE layer, one per recipe suffix slot. The split
// layout fills all three ({gate, up, down}); a fused-gate_up layout fills two and leaves
// the tail slot empty (tensor == nullptr). Unbound layers (dense, or non-MoE) stay false.
struct LayerExperts {
    bool bound = false;
    ExpertTensorRef proj[MoeRecipe::max_exps];
};

class ExpertStreamSource final : public IExpertSource {
public:
    ExpertStreamSource() = default;
    ~ExpertStreamSource() override;

    ExpertStreamSource(const ExpertStreamSource &) = delete;
    ExpertStreamSource & operator=(const ExpertStreamSource &) = delete;

    // Build buffers, rebind the bound layers' expert tensors onto them, and start the
    // I/O pool. `layers` is indexed by layer id (unbound entries are skipped).
    // Returns false on any allocation/open failure or an inconsistent tensor.
    bool
    init(const std::string & gguf_path, int n_expert, std::vector<LayerExperts> layers, const MoeStreamConfig & cfg);

    // IExpertSource
    bool load_layer(int il, const int32_t * ids, int n_ids) override;
    void prefetch(int il, const int32_t * ids, int n_ids) override;
    Stats stats() const override;

    // Register the process-global expert-ready hook so the CPU matmul blocks per expert
    // until its slice is resident. Only meaningful in overlap mode; a no-op if the fork
    // hook was not compiled in. Paired with shutdown(), which unregisters it.
    void enable_overlap_hook();

    // True once an async read has failed or the source is shutting down. Wired to
    // llama_set_abort_callback so a mid-decode I/O failure aborts the graph cleanly
    // instead of computing on a half-read expert.
    bool fatal() const { return fatal_.load(std::memory_order_acquire); }

    // Explicitly set the cache budget in bytes and evict down to it immediately (clamped to the
    // full expert-set size). PRECONDITION: no decode in flight — the caller must not be inside a
    // load_layer/generate. Intended for an app's memory-pressure callback (Android onTrimMemory)
    // and exercised by the shrink gate. Clamped up: an explicit raise also lifts the grow ceiling.
    void set_cache_budget(size_t bytes);

    void shutdown();

private:
    struct IoJob {
        void * dst = nullptr;
        uint64_t off = 0;
        uint64_t nbytes = 0;
        int32_t flag = -1; // overlap: index into ready_ to publish on completion; -1 = serial
    };

    // One readiness cell per (projection, expert). A cell is "ready for the layer in flight"
    // exactly when gen == async_gen_; padded to a cache line so the compute threads polling it
    // do not false-share the LRU/job bookkeeping the load thread mutates alongside.
    struct alignas(64) ReadyFlag {
        std::atomic<uint32_t> gen{0};
    };

    bool read_slice(int lane, void * dst, uint64_t off, uint64_t nbytes);
    void io_drain(int lane, uint64_t my_gen);
    void io_worker(int lane);

    // Speculative prefetch (temporal): drain queued spec reads on an idle lane, and integrate /
    // discard them on the eval thread at the next real load. See docs/prefetch.md.
    void drain_spec(int lane, uint64_t worker_seen);
    void quiesce_spec();
    void release_entry_pages(int32_t id);

    // One sequential buffered sweep over the file's non-expert byte ranges (header, embeddings,
    // attention, norms, lm_head) to populate the kernel page cache at load time, so the mmap'd
    // dense tensors do not demand-fault 4 KiB at a time inside the first decodes. Best-effort:
    // a read failure only leaves the corresponding pages cold.
    void warm_dense_regions(const std::string & gguf_path);

    // Adaptive sizing (cache_auto): re-probe device memory (throttled) and nudge cache_max_ so it
    // tracks free RAM. Eval-thread only, called in the mgmt section of load_layer; the eviction
    // loop that follows drains any shrink.
    void adapt_cache_budget();

    bool load_layer_async(int il, const int32_t * ids, int n_ids); // overlap path

    static void c_expert_ready(const ggml_tensor * src0, int expert, void * user_data);
    void on_expert_ready(const ggml_tensor * src0, int expert);

    // LRU helpers (active only when cache_max_ > 0)
    void lru_unlink(int32_t id);
    void lru_push_front(int32_t id);
    void lru_push_back(int32_t id);
    size_t entry_bytes(int il) const;
    void evict_tail();

    bool active_ = false;
    bool o_direct_ = false;
    bool load_all_ = false;
    bool overlap_ = false;
    bool prefetch_sync_ = false; // test only: drain prefetch reads synchronously (serial mode)
    int n_layer_ = 0;
    int n_expert_ = 0;
    uint64_t fsize_ = 0;
    size_t align_ = 4096;

    std::vector<LayerExperts> layers_;

    // shared-slot mode (cache off): one full-size slot per projection, reused by layers
    void * slot_[MoeRecipe::max_exps] = {nullptr, nullptr, nullptr};
    size_t slot_sz_[MoeRecipe::max_exps] = {0, 0, 0};

    // LRU mode (cache on): per-(layer, projection) reserved buffers
    size_t cache_max_ = 0; // live budget in bytes; mutated at runtime when cache_auto_
    size_t page_ = 4096;

    // Adaptive sizing (cache_auto): budget derived from device memory at init, then re-checked
    // during generation on the eval thread so it tracks free RAM. cache_target_ is the init budget,
    // the ceiling for grow-back; cache_floor_ is the RAM to leave free; total_expert_bytes_ caps it.
    bool cache_auto_ = false;
    size_t cache_floor_ = 0;
    size_t cache_target_ = 0;
    size_t total_expert_bytes_ = 0;
    size_t cache_hard_cap_ = 0; // upper bound on the auto budget (ceiling ∧ full expert-set size)
    unsigned probe_tick_ = 0;   // throttles the mem_available re-probe (once per N load_layer calls)
    long long cache_resizes_ = 0;
    std::vector<void *> lbuf_[MoeRecipe::max_exps];
    std::vector<size_t> lbuf_sz_[MoeRecipe::max_exps];
    std::vector<uint8_t> cvalid_;
    std::vector<int32_t> cprev_, cnext_;
    std::vector<uint32_t> cstamp_;
    std::vector<uint8_t> cspec_; // 1 if this resident entry was filled speculatively, until first hit
    int32_t chead_ = -1, ctail_ = -1;
    uint32_t cgen_ = 0;
    size_t cresident_ = 0;
    long long chits_ = 0, clookups_ = 0;

    // ── speculative prefetch queue (all fields guarded by io_mtx_) ──
    // prefetch() (eval thread) commits pages and enqueues per-projection reads tagged with the
    // entry id; workers drain them on idle lanes; quiesce_spec() (eval thread, at the next real
    // load) integrates fully-read entries into the cache and releases the rest. spec_gen_ bumps to
    // cancel a round. All LRU mutation stays on the eval thread — workers only read bytes.
    std::vector<IoJob> spec_jobs_;
    size_t spec_next_ = 0;
    uint64_t spec_gen_ = 0;
    size_t spec_inflight_ = 0;
    std::vector<int32_t> spec_done_;      // entry ids whose every projection read completed
    std::vector<int32_t> spec_touched_;   // entry ids queued this round (for cleanup at quiesce)
    std::vector<int32_t> spec_remaining_; // per entry id: projection reads still pending (0 = none)
    std::atomic<long long> spec_read_bytes_{0};
    std::atomic<long long> spec_experts_{0};
    std::atomic<long long> spec_useful_{0};

    std::vector<uint8_t> seen_; // per-load dedup scratch [n_expert]

    // I/O lane pool
    int io_threads_ = 1;
    std::vector<pio::fd_t> fds_;
    std::vector<pio::fd_t> fds_buf_;
    std::vector<void *> bounces_;
    std::vector<size_t> bounce_sz_;
    std::vector<std::thread> io_pool_;
    std::vector<IoJob> jobs_;
    std::mutex io_mtx_;
    std::condition_variable io_cv_, io_cv_done_;
    uint64_t batch_gen_ = 0;
    size_t batch_njobs_ = 0;
    size_t next_idx_ = 0, done_cnt_ = 0;
    bool io_stop_ = false;
    std::atomic<bool> io_err_{false};
    uint32_t batch_flag_gen_ = 0; // async_gen_ of the batch in flight; snapshot under io_mtx_ at publish

    std::atomic<long long> read_bytes_{0};
    std::atomic<long long> read_ns_{0};
    std::atomic<long long> io_syscall_ns_{0};
    std::atomic<long long> mgmt_ns_{0}; // staging-section time: vm commit + evict + LRU bookkeeping

    // ── overlap mode: one layer in flight at a time (guaranteed by graph order) ──
    // A ReadyFlag is ready iff its gen == async_gen_; the load thread bumps async_gen_ per
    // layer, marks cache hits ready immediately, and workers mark misses ready after the read.
    // The compute threads look a captured expert tensor up in texp_, then spin/wait on its flag.
    std::vector<ReadyFlag> ready_; // size max_exps * n_expert_; idx = p*n_expert_+e
    std::atomic<uint32_t> async_gen_{0};
    std::atomic<int> cur_il_{-1}; // layer whose experts the hook may wait on
    std::atomic<bool> fatal_{false};
    std::mutex ready_mtx_;
    std::condition_variable ready_cv_;
    std::atomic<long long> stall_ns_{0};              // summed across all stalling compute threads
    std::unordered_map<const void *, uint32_t> texp_; // expert tensor* -> (il<<8)|p, built in init
    std::vector<int> staged_;                         // per-load sorted unique expert scratch
    bool hook_registered_ = false;
};

} // namespace bmoe
