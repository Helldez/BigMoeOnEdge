// The prefill device as the session sees it: which device, what was moved there, and where the next
// graph runs.
//
// PrefillDevice (layer weights and model state) and DeviceExpertArena (streamed experts) are the
// mechanisms; this is their composition for one session. It picks the weights to move from the
// capture graph, sizes the arena from what the streamer was given, redoes the context's compute
// reservation with the weights in place, and then offers the session four things: place the next
// graph, run a decode there, clear the moved state, and read the arena's counters. The session keeps
// only the decision of WHICH graphs go to the device (PrefillDeviceConfig::min_tokens).
#pragma once

#include "prefill_device.h"
#include "../moe/device_arena.h"
#include "../moe/expert_stream_source.h"

#include "bmoe/config.h"

#include "ggml-backend.h"
#include "llama.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bmoe {

class RouterHook;

namespace detail {

class PrefillPath {
public:
    // Looks `name` up in the backend registry. Null, with `err` naming the devices found, when it is
    // absent: both when the build lacks the backend and when the backend found no hardware (a phone
    // without the accelerator's driver registers nothing), so the message names neither cause.
    static ggml_backend_dev_t find_device(const std::string & name, std::string & err);

    explicit PrefillPath(ggml_backend_dev_t dev) : devs_{dev, nullptr} {}
    PrefillPath(const PrefillPath &) = delete;
    PrefillPath & operator=(const PrefillPath &) = delete;

    // The null-terminated list for llama_model_params::devices. It must outlive the model load.
    ggml_backend_dev_t * model_devices() { return devs_; }

    // With streaming, the arena is sized from exactly what the streamer is given, so the session hands
    // a copy over before the streamer takes it.
    void keep_stream_layout(const std::vector<LayerExperts> & layers, const std::vector<std::string> & shards) {
        arena_layers_ = layers;
        arena_shards_ = shards;
    }

    // Everything after load: learn the layer weights from one graph (streaming already ran that
    // capture; without it, run it here), give them a device copy or an arena, move the model state,
    // and redo the compute reservation. `ctx`'s memory is left empty.
    bool open(llama_context * ctx,
              const llama_vocab * vocab,
              RouterHook & hook,
              const PrefillDeviceConfig & cfg,
              const MoeStreamConfig & moe,
              int n_layer_streamed,
              std::string & err);

    // Placement of the NEXT graph: the layer weights, the streamed experts and the hook move together.
    void place(bool on_device);
    // One llama_decode, on whichever side place() last chose. On the device the arena is started
    // before and drained after, and a failed expert read fails the decode: the graph would have
    // computed on a slot that never filled.
    int decode(llama_context * ctx, const llama_batch & b);
    // For every llama_memory_clear(data) of the target context: llama.cpp clears the buffers it
    // allocated, which the moved model state no longer lives in.
    void clear_state();

    uint64_t arena_read_bytes() const { return arena_ ? arena_->read_bytes() : 0; }
    double arena_stall_seconds() const { return arena_ ? arena_->stall_seconds() : 0.0; }

private:
    bool open_arena(const PrefillDeviceConfig & cfg,
                    const MoeStreamConfig & moe,
                    const std::vector<std::vector<ggml_tensor *>> & dense_per_layer,
                    std::string & err);
    bool reserve_on_device(llama_context * ctx, const llama_vocab * vocab, int n_tokens);

    ggml_backend_dev_t devs_[2];
    RouterHook * hook_ = nullptr;
    std::vector<LayerExperts> arena_layers_;
    std::vector<std::string> arena_shards_;
    // Declared before the arena so it is destroyed after it: the arena hands the experts back to the
    // streamer's binding first, then the device copy goes.
    std::unique_ptr<PrefillDevice> dev_;
    std::unique_ptr<DeviceExpertArena> arena_;
};

} // namespace detail
} // namespace bmoe
