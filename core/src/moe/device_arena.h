// Streamed experts for a prefill graph that runs on a device.
//
// A model larger than RAM cannot give the device a resident copy of its experts, and a prefill graph
// hundreds of tokens wide needs nearly every expert of every layer anyway. So the device gets TWO
// layer-sized slots of expert weights and every MoE layer's expert tensors are bound to one of them,
// alternating. While the device computes layer k out of one slot, a pool of loader threads fills the
// other with layer k+1: read each expert's bytes from the gguf, hand them to the backend through
// ggml_backend_tensor_set on a per-expert view (which is where a repacking backend lays them out in
// its own format). One flash pass per graph, hidden behind compute when compute is the longer of the
// two.
//
// The pacing point is the layer's routing node, which the router hook already knows how to find and
// isolate: when the scheduler stops there, every op of layer k-1 has finished, so the slot k+1 goes
// into is free, and nothing of layer k that needs experts has run yet. barrier() queues the next fill
// and waits for the current one. The routing itself is not read: at this width it selects nearly
// every expert, and on the device it is not in host memory to read.
//
// The layer's other weights (attention, norms, shared experts) can ride the same two-slot scheme
// (init_dense). They are resident on the host already, so filling them is a copy, not a read; what
// they cost the device is two layers of them instead of all of them, which on unified memory is the
// difference between fitting the device's session and not. They are needed before the layer's
// routing, so they are paced one layer ahead, at the last node of the previous layer (learned from
// the capture graph, since no node name is common to every architecture).
//
// Decode never sees any of this. place(false) hands every expert tensor back to the binding it had
// (the streamer's), and the slots are not a cache: each graph refills them from layer 0.
#pragma once

#include "expert_stream_source.h"
#include "../io/file_reader.h"

#include "ggml-backend.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace bmoe {

class DeviceExpertArena {
public:
    DeviceExpertArena() = default;
    ~DeviceExpertArena();
    DeviceExpertArena(const DeviceExpertArena &) = delete;
    DeviceExpertArena & operator=(const DeviceExpertArena &) = delete;

    // `layers` is indexed by layer id, as the streamer receives it; unbound entries are dense layers
    // and get no slot. Layers may differ in an expert tensor's type (quantisers keep some ffn_down in
    // more bits): each projection gets a region of the slot sized for its largest variant, and one
    // slot tensor per variant at that address. `threads` loader threads, each with its own lane.
    bool init(ggml_backend_dev_t dev,
              const std::vector<std::string> & shard_paths,
              const std::vector<LayerExperts> & layers,
              int threads,
              bool direct,
              std::string & err);

    // Also carry each layer's non-expert weights through two slots. `per_layer[il]` lists layer il's
    // weights (host resident, contiguous); layers may differ in what they hold. Call after init.
    //
    // `non_matrix` are the weights some op reads other than as a matmul's matrix (RouterHook::
    // non_matrix_weights). A backend may keep a WEIGHTS buffer in a form only its matmul kernels
    // address (Hexagon maps it for DMA only when DMA64 is on, and most other kernels refuse it), so
    // these ride a second pair of slots marked as plain memory, in their own type. The matrices keep
    // the WEIGHTS slots, where the backend lays them out for its matmul.
    bool init_dense(ggml_backend_dev_t dev,
                    const std::vector<std::vector<ggml_tensor *>> & per_layer,
                    const std::unordered_set<const ggml_tensor *> & non_matrix,
                    std::string & err);

    // Bind every expert tensor to its slot (true) or back to what it had before (false). The host
    // binding is taken at the moment of the swap, so whatever the streamer bound stays authoritative.
    void place(bool on_device);

    // Start of a device graph: queue the first two layers. Each graph refills from layer 0.
    void begin_graph();
    // At layer il's routing node: queue the next layer into the slot the previous one just freed,
    // then wait until il's experts are in place. A layer with no slot passes straight through.
    void barrier(int il);
    // At the last node of layer il: its dense slot is free, so queue layer il+2 into it, and wait
    // for layer il+1's. With no dense slots this is a no-op.
    void dense_barrier(int il);
    // End of a device graph: wait out anything still in flight (only a graph that stopped early
    // leaves any), so the next graph starts from empty queues.
    void end_graph();

    // Test hook: sleep before each layer's first upload (PrefillDeviceConfig::test_load_delay_us).
    void set_test_delay_us(int us) { test_delay_us_ = us; }

    bool failed() const { return failed_.load(); }
    uint64_t read_bytes() const { return read_bytes_.load(); }
    double stall_seconds() const { return stall_ns_.load() * 1e-9; }
    size_t slot_bytes() const { return slot_bytes_; }
    size_t dense_slot_bytes() const { return dense_slot_bytes_; }
    int dense_converted() const { return dense_converted_; }
    size_t dense_converted_bytes() const { return dense_converted_bytes_; }
    bool has_dense() const { return !dense_.empty(); }
    int n_layers() const { return (int) order_.size(); }

private:
    struct Proj {
        uint64_t file_off = 0;
        uint64_t nb2 = 0;
        int file_idx = 0;
    };
    struct Layer {
        int il = -1;
        ggml_tensor * t[MoeRecipe::max_exps] = {};
        Proj proj[MoeRecipe::max_exps];
        ggml_backend_buffer_t host_buffer[MoeRecipe::max_exps] = {};
        void * host_data[MoeRecipe::max_exps] = {};
        void * host_extra[MoeRecipe::max_exps] = {};
        int variant[MoeRecipe::max_exps] = {}; // which slot tensor of the projection this layer uses
    };
    // A slot tensor for one (type, shape) variant of a projection, and a view per expert into it.
    struct Twin {
        ggml_tensor * t = nullptr;
        std::vector<ggml_tensor *> views;
    };
    struct Task {
        bool dense = false;
        int k = 0; // expert task: index into order_; dense task: layer id
        int p = 0; // expert task: projection; dense task: tensor index within the layer
        int e = 0;
    };
    struct DenseLayer {
        std::vector<ggml_tensor *> t;    // the model's tensors
        std::vector<ggml_tensor *> twin; // their places in slot il % 2
        std::vector<const void *> src;   // host bytes, taken at init: t is rebound while filling
        std::vector<ggml_backend_buffer_t> host_buffer;
        std::vector<void *> host_data, host_extra;
        // A weight whose type the device's matmul does not take is carried in one it does (see
        // init_dense); its converted bytes live here, and the swap changes type and strides too.
        std::vector<std::vector<uint8_t>> converted;
        std::vector<ggml_type> host_type;
        std::vector<std::array<size_t, GGML_MAX_DIMS>> host_nb;
    };

    void worker(int lane);
    void schedule(int k);        // caller holds mu_
    void schedule_dense(int il); // caller holds mu_

    std::vector<Layer> order_;    // bound layers in graph order
    std::vector<int> k_of_layer_; // layer id -> index into order_, -1 for dense layers
    int n_expert_ = 0;
    int n_proj_ = 0;

    ggml_context * ctx_ = nullptr;
    ggml_backend_buffer_t slot_buf_[2] = {};
    // twins_[s][p][variant]: every variant of projection p in slot s sits at the same address. Built
    // once, views included; a repacking backend keeps per-tensor state for each, so recreating them
    // per fill would grow without bound.
    std::vector<Twin> twins_[2][MoeRecipe::max_exps];
    size_t slot_bytes_ = 0;

    std::vector<std::unique_ptr<FileReader>> readers_;
    std::vector<void *> staging_; // one per lane
    std::vector<std::thread> threads_;

    std::mutex mu_;
    std::condition_variable cv_work_;
    std::condition_variable cv_done_;
    std::deque<Task> queue_;
    std::vector<int> remaining_;  // per order_ index: tasks not yet finished for this graph
    std::vector<bool> scheduled_; // per order_ index, this graph
    int in_flight_ = 0;
    bool stop_ = false;
    bool on_device_ = false;
    int test_delay_us_ = 0;

    std::vector<DenseLayer> dense_; // by layer id; empty when init_dense was not called
    ggml_context * dense_ctx_ = nullptr;
    ggml_backend_buffer_t dense_buf_[2] = {};      // matmul matrices, usage WEIGHTS
    ggml_backend_buffer_t dense_data_buf_[2] = {}; // every other layer weight, plain memory
    size_t dense_slot_bytes_ = 0;
    int dense_converted_ = 0;
    size_t dense_converted_bytes_ = 0;
    std::vector<int> dense_remaining_;
    std::vector<bool> dense_scheduled_;
    std::vector<bool> dense_waited_; // this graph: someone waited for the layer before it ran

    std::atomic<bool> failed_{false};
    std::atomic<uint64_t> read_bytes_{0};
    std::atomic<uint64_t> stall_ns_{0};
};

} // namespace bmoe
