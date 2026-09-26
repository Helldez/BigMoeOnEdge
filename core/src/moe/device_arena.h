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
// Decode never sees any of this. place(false) hands every expert tensor back to the binding it had
// (the streamer's), and the slots are not a cache: each graph refills them from layer 0.
#pragma once

#include "expert_stream_source.h"
#include "../io/file_reader.h"

#include "ggml-backend.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bmoe {

class DeviceExpertArena {
public:
    DeviceExpertArena() = default;
    ~DeviceExpertArena();
    DeviceExpertArena(const DeviceExpertArena &) = delete;
    DeviceExpertArena & operator=(const DeviceExpertArena &) = delete;

    // `layers` is indexed by layer id, as the streamer receives it; unbound entries are dense layers
    // and get no slot. Every bound layer must have the same expert tensor shapes, since they share
    // two slots. `threads` loader threads, each with its own read lane.
    bool init(ggml_backend_dev_t dev,
              const std::vector<std::string> & shard_paths,
              const std::vector<LayerExperts> & layers,
              int threads,
              bool direct,
              std::string & err);

    // Bind every expert tensor to its slot (true) or back to what it had before (false). The host
    // binding is taken at the moment of the swap, so whatever the streamer bound stays authoritative.
    void place(bool on_device);

    // Start of a device graph: queue the first two layers. Each graph refills from layer 0.
    void begin_graph();
    // At layer il's routing node: queue the next layer into the slot the previous one just freed,
    // then wait until il's experts are in place. A layer with no slot passes straight through.
    void barrier(int il);
    // End of a device graph: wait out anything still in flight (only a graph that stopped early
    // leaves any), so the next graph starts from empty queues.
    void end_graph();

    // Test hook: sleep before every expert upload (PrefillDeviceConfig::test_load_delay_us).
    void set_test_delay_us(int us) { test_delay_us_ = us; }

    bool failed() const { return failed_.load(); }
    uint64_t read_bytes() const { return read_bytes_.load(); }
    double stall_seconds() const { return stall_ns_.load() * 1e-9; }
    size_t slot_bytes() const { return slot_bytes_; }
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
    };
    struct Task {
        int k = 0; // index into order_
        int p = 0;
        int e = 0;
    };

    void worker(int lane);
    void schedule(int k); // caller holds mu_

    std::vector<Layer> order_;    // bound layers in graph order
    std::vector<int> k_of_layer_; // layer id -> index into order_, -1 for dense layers
    int n_expert_ = 0;
    int n_proj_ = 0;

    ggml_context * ctx_ = nullptr;
    ggml_backend_buffer_t buf_ = nullptr;
    ggml_tensor * slot_[2][MoeRecipe::max_exps] = {};
    // Per-expert views into each slot: views_[s][p][e]. Built once; a repacking backend keeps
    // per-tensor state for each, so recreating them per fill would grow without bound.
    std::vector<ggml_tensor *> views_[2][MoeRecipe::max_exps];
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

    std::atomic<bool> failed_{false};
    std::atomic<uint64_t> read_bytes_{0};
    std::atomic<uint64_t> stall_ns_{0};
};

} // namespace bmoe
