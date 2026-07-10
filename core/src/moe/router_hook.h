// The eval-callback adapter: the only bridge between llama.cpp's compute graph and the
// expert streamer, and it uses only the public callback ABI (llama_context_params.cb_eval).
//
// Two phases share one callback:
//
//   Capture — during a one-token warm-up decode (experts still mmap-resident), every
//   graph node is offered to the callback in its "ask" pass. We scan each node's sources
//   for the recipe's expert weight tensors (named blk.<il>.<suffix>.weight) and record
//   the live ggml_tensor pointers per (layer, projection). No node is isolated. The
//   runtime then hands these to ExpertStreamSource::init, which rebinds them.
//
//   Stream — for real generation, we ask for only the routing nodes ffn_moe_topk-<il>.
//   ggml computes and synchronizes each alone, then calls back with the selected expert
//   ids materialized; we gather them respecting the view strides and call load_layer()
//   so the routed slices land before that layer's expert matmul runs.
#pragma once

#include "bmoe/recipe.h"
#include "expert_stream_source.h"

#include <cstdint>
#include <vector>

struct ggml_tensor;

namespace bmoe {

class RouterHook {
public:
    RouterHook(const MoeRecipe & recipe, int n_layer);

    // Install as ctx_params.cb_eval / cb_eval_user_data before context creation.
    static bool c_eval(ggml_tensor * t, bool ask, void * user_data);

    void begin_capture();  // switch to capture; clears prior records
    void end_capture();    // stop recording (called after the warm-up decode)

    // After capture, the tensors found per (layer, projection). Entry.bound is false for
    // layers that produced no expert tensors (dense layers). file_off/nb2 are filled in
    // by the runtime from the gguf; here only .tensor is set.
    const std::vector<LayerExperts> & captured() const { return captured_; }
    std::vector<LayerExperts> &       captured()       { return captured_; }

    void set_source(IExpertSource * src) { source_ = src; }  // enter stream mode

private:
    bool on_eval(ggml_tensor * t, bool ask);

    const MoeRecipe & recipe_;
    int               n_layer_ = 0;
    bool              capturing_ = false;
    IExpertSource *   source_ = nullptr;  // non-null → stream mode
    std::vector<LayerExperts> captured_;
    std::vector<int32_t>      gathered_;  // reused scratch for stream-mode id gather
};

} // namespace bmoe
