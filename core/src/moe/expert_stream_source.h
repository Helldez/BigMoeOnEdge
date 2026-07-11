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
    Stats stats() const override;

    void shutdown();

private:
    struct IoJob {
        void * dst = nullptr;
        uint64_t off = 0;
        uint64_t nbytes = 0;
    };

    bool read_slice(int lane, void * dst, uint64_t off, uint64_t nbytes);
    void io_drain(int lane, uint64_t my_gen);
    void io_worker(int lane);

    // LRU helpers (active only when cache_max_ > 0)
    void lru_unlink(int32_t id);
    void lru_push_front(int32_t id);
    size_t entry_bytes(int il) const;
    void evict_tail();

    bool active_ = false;
    bool o_direct_ = false;
    bool load_all_ = false;
    int n_layer_ = 0;
    int n_expert_ = 0;
    uint64_t fsize_ = 0;
    size_t align_ = 4096;

    std::vector<LayerExperts> layers_;

    // shared-slot mode (cache off): one full-size slot per projection, reused by layers
    void * slot_[MoeRecipe::max_exps] = {nullptr, nullptr, nullptr};
    size_t slot_sz_[MoeRecipe::max_exps] = {0, 0, 0};

    // LRU mode (cache on): per-(layer, projection) reserved buffers
    size_t cache_max_ = 0;
    size_t page_ = 4096;
    std::vector<void *> lbuf_[MoeRecipe::max_exps];
    std::vector<size_t> lbuf_sz_[MoeRecipe::max_exps];
    std::vector<uint8_t> cvalid_;
    std::vector<int32_t> cprev_, cnext_;
    std::vector<uint32_t> cstamp_;
    int32_t chead_ = -1, ctail_ = -1;
    uint32_t cgen_ = 0;
    size_t cresident_ = 0;
    long long chits_ = 0, clookups_ = 0;

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

    std::atomic<long long> read_bytes_{0};
    std::atomic<long long> read_ns_{0};
    std::atomic<long long> io_syscall_ns_{0};
};

} // namespace bmoe
