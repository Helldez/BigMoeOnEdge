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
#include "bmoe/decode_trace.h"
#include "bmoe/recipe.h"
#include "../io/platform_io.h"
#include "../io/file_reader.h"

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

// One dense (non-expert) weight tensor to read once via O_DIRECT and rebind onto our own
// anonymous buffer, for the --dense-odirect experiment. Unlike an expert slice, a dense tensor is
// read whole and contiguous: `size` bytes from `file_off`, matching its gguf on-disk layout.
struct DenseTensorRef {
    ggml_tensor * tensor = nullptr; // persistent weight tensor whose ->data we rebind
    uint64_t file_off = 0;          // absolute file offset of the tensor's data
    uint64_t size = 0;              // its data size in bytes (== ggml_nbytes)
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

    // Supply the dense (non-expert) weight tensors to read via O_DIRECT and rebind, for the
    // --dense-odirect experiment. Call BEFORE init (which does the read); a no-op unless
    // cfg.dense_odirect is set. The runtime builds the list from the captured weight leaves and the
    // gguf offsets. See docs/pressure.md.
    void set_dense_tensors(std::vector<DenseTensorRef> dense) { dense_tensors_ = std::move(dense); }

    // IExpertSource
    bool load_layer(int il, const int32_t * ids, int n_ids) override;
    void prefetch(int il, const int32_t * ids, int n_ids) override;
    void settle_spec() override;
    void query_residency(int il, const int32_t * ids, int n_ids, uint8_t * out) const override;
    uint64_t expert_bytes(int il) const override;
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

    // ── I/O trace (diagnostics; see bmoe/decode_trace.h) ────────────────────────────
    // When on, every read_slice records one row. Rows are appended under a dedicated leaf mutex
    // (reads happen on N lanes at once), so this costs a lock per read and is off by default.
    // take_io_trace_rows moves the buffer out; the caller stamps the frame it belongs to.
    void set_io_trace(bool on);
    void take_io_trace_rows(std::vector<IoTraceRow> & out);

    void shutdown();

private:
    struct IoJob {
        void * dst = nullptr;
        uint64_t off = 0;
        uint64_t nbytes = 0;
        int32_t flag = -1; // overlap: index into ready_ to publish on completion; -1 = serial
        // Which (layer, expert, projection) this read serves. Known at every enqueue site and
        // otherwise thrown away; carried so the I/O trace can attribute a read without the read
        // path having to guess. Inert unless the trace is on.
        int32_t expert = -1;
        int16_t layer = -1;
        int8_t proj = -1;
        uint8_t spec = 0; // 1 if enqueued speculatively by prefetch
    };

    // One readiness cell per (projection, expert). A cell is "ready for the layer in flight"
    // exactly when gen == async_gen_; padded to a cache line so the compute threads polling it
    // do not false-share the LRU/job bookkeeping the load thread mutates alongside.
    struct alignas(64) ReadyFlag {
        std::atomic<uint32_t> gen{0};
    };

    // `j` carries the read AND (for the trace) what it serves; `lane` is who is doing it.
    bool read_slice(int lane, const IoJob & j);
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

    // Experiment (--dense-odirect): read every dense tensor once via O_DIRECT into an aligned anon
    // buffer and rebind its ->data onto it, so the dense weights are anonymous (a reclaim swaps
    // them to zram instead of dropping them to a 4 KiB flash refault). Done once in init, on lane 0,
    // before the workers start; the buffers are freed in shutdown(). Returns false on any failure.
    bool read_dense_odirect();

    // Adaptive sizing (cache_auto): re-probe device memory (throttled) and nudge cache_max_ so it
    // tracks free RAM. Eval-thread only, called in the mgmt section of load_layer; the eviction
    // loop that follows drains any shrink.
    void adapt_cache_budget();

    // Pressure sensing (cache_dynamic): sample (throttled) how much of the cache the kernel still
    // has in RAM, walking the LRU cold end — the pages reclaim takes first, so a dip here is the
    // earliest evidence the budget outgrew what the device concedes. Sets resident_frac_ for the
    // governor; changes no budget itself. Eval-thread only (it walks the LRU list).
    void sample_residency();

    // Companion sensor for the OTHER half of memory: the dense weights, which are mmap'd file-backed
    // and reclaimed by being dropped (never swapped), so resident_frac — which watches only our anon
    // cache — is blind to them. A stratified mincore over the dense file ranges says how much of the
    // model itself the kernel still has. When this falls while resident_frac holds, the faults are
    // the weights, not the cache, and shrinking the cache cannot help. Sets dense_resident_frac_.
    void sample_dense_residency();

    // Accumulate one token's routed working set. Eval-thread only, called per layer load.
    void account_demand(int il, int n_unique);

    bool load_layer_async(int il, const int32_t * ids, int n_ids); // overlap path

    static void c_expert_ready(const ggml_tensor * src0, int expert, void * user_data);
    void on_expert_ready(const ggml_tensor * src0, int expert);

    // LRU helpers (active only when cache_max_ > 0)
    void lru_unlink(int32_t id);
    void lru_push_front(int32_t id);
    void lru_push_back(int32_t id);
    size_t entry_bytes(int il) const;
    void evict_tail();

    // I/O trace buffer. Its own leaf mutex, never held across a read: the lanes append
    // concurrently, and the eval thread swaps the buffer out between decodes.
    bool io_trace_on_ = false;
    std::mutex io_trace_mtx_;
    std::vector<IoTraceRow> io_trace_rows_;

    bool active_ = false;
    bool load_all_ = false;
    bool overlap_ = false;
    bool prefetch_sync_ = false; // test only: drain prefetch reads synchronously (serial mode)
    int n_layer_ = 0;
    int n_expert_ = 0;
    size_t align_ = 4096;

    // The positioned reader that owns the fd pool, bounces and O_DIRECT decision. Expert slices are
    // read through it; the dense-weights loader constructs its own, so their O_DIRECT choices are
    // independent (see docs/architecture.md). file_size() replaces the old fsize_ member.
    FileReader reader_;

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
    unsigned probe_tick_ = 0;   // throttles the periodic sensing (once per N load_layer calls)
    long long cache_resizes_ = 0;

    // ── pressure sensing (cache_dynamic): the engine senses, bmoe::CacheGovernor decides ──
    // Sampling the whole cache every layer would cost more than the reclaim it detects, so a probe
    // reads a bounded window of the cold end. Cost lands in mgmt_ns_, where a regression is visible
    // rather than hidden in the compute residual.
    static constexpr unsigned residency_probe_every = 128; // load_layer calls between probes (~2-3 tokens)
    static constexpr int residency_sample_entries = 16;    // coldest entries read per probe
    static constexpr int dense_sample_pages = 256;         // stratified probe points over the dense weights
    bool cache_dynamic_ = false;
    bool dense_odirect_ = false; // --dense-odirect: read dense weights via O_DIRECT into our buffers
    std::vector<DenseTensorRef> dense_tensors_; // set before init; read+rebound when dense_odirect_
    std::vector<void *> dense_bufs_;            // the anon buffers backing them; freed in shutdown()
    std::vector<size_t> dense_buf_sz_;          // dense_bufs_[i]'s byte size, for the anon-residency sample
    double resident_frac_ = -1.0;               // last sampled cache residency; -1 = never measured
    double dense_resident_frac_ = -1.0; // last sampled dense-weight residency; -1 = never measured

    // The dense weights' byte ranges in the file (complement of the expert ranges), and the mmap VMAs
    // that back them — resolved once, lazily, from /proc/self/maps. Empty vma set = maps unreadable,
    // which keeps dense_resident_frac_ at -1 rather than inventing a number.
    std::string gguf_path_;
    std::vector<std::pair<uint64_t, uint64_t>> dense_ranges_;
    std::vector<pio::MappedRegion> dense_vmas_;
    bool dense_vmas_tried_ = false;

    // Two measured demands, and the difference between them decides the whole policy.
    //
    // token_demand_: the bytes routed between two visits to the same layer — one token's working
    // set. Below it the cache stops holding anything BETWEEN tokens, so its hit rate collapses to
    // inter-token correlation only. Informative (it says where hits start), never a floor: measured
    // on gpt-oss it is 1815 MiB, more than the device concedes, so flooring there just pins the
    // budget inside a war it cannot win. See bmoe/cache_governor.h.
    //
    // layer_demand_: the largest single layer's routed bytes. THIS is the floor, because it is
    // mechanical — the cache has to hold the layer it is staging right now.
    size_t demand_accum_ = 0;
    size_t token_demand_ = 0;
    size_t layer_demand_accum_ = 0;
    size_t layer_demand_ = 0;
    int last_il_ = -1;
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

    std::atomic<long long> read_ns_{0};
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
