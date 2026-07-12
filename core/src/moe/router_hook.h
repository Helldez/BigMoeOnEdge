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

    void begin_capture(); // switch to capture; clears prior records
    void end_capture();   // stop recording (called after the warm-up decode)

    // After capture, the tensors found per (layer, projection). Entry.bound is false for
    // layers that produced no expert tensors (dense layers). file_off/nb2 are filled in
    // by the runtime from the gguf; here only .tensor is set.
    const std::vector<LayerExperts> & captured() const { return captured_; }
    std::vector<LayerExperts> & captured() { return captured_; }

    void set_source(IExpertSource * src) { source_ = src; } // enter stream mode

    // Temporal prefetch depth K: while streaming layer l, hint the source to read ahead the
    // experts the previous token used at layers l+1..l+K. 0 (default) disables it.
    void set_prefetch_layers(int k) { prefetch_layers_ = k; }

    // Speculative gating: predict the next MoE layer's experts by running its router on the
    // current layer's hidden state, and prefetch them. Needs n_expert_used (top-k) and the rms
    // epsilon (for architectures whose router normalises its input). Call before begin_capture()
    // so the gate weights are harvested. Only the predicted ids feed prefetch, so a wrong
    // prediction wastes a read but never changes output.
    void set_spec_gate(bool on, int n_expert_used, float rms_eps);

    // Speculative-gating prediction accuracy (cumulative): how many predicted experts a later
    // routing actually used, over how many predicted. Read by the runtime for telemetry.
    long long spec_pred_total() const { return spec_pred_total_; }
    long long spec_pred_hit() const { return spec_pred_hit_; }

private:
    bool on_eval(ggml_tensor * t, bool ask);
    void predict_and_prefetch(const ggml_tensor * node, int il);

    // Stored by value, not by reference: the caller often constructs us from a temporary
    // (a `cond ? *ptr : MoeRecipe{}` ternary yields a prvalue even when ptr is non-null),
    // which would leave a reference member dangling. The struct is just a handful of string
    // literal pointers, so copying is cheap and keeps the suffixes valid for our lifetime.
    MoeRecipe recipe_;
    int n_layer_ = 0;
    bool capturing_ = false;
    IExpertSource * source_ = nullptr; // non-null → stream mode
    std::vector<LayerExperts> captured_;
    std::vector<int32_t> gathered_; // reused scratch for stream-mode id gather

    // Temporal prefetch: K, and the previous token's routed experts per layer (last-token row
    // during prefill). Empty when prefetch is off or a layer has not been seen yet.
    int prefetch_layers_ = 0;
    std::vector<std::vector<int32_t>> prev_ids_;

    // Speculative gating.
    bool spec_gate_ = false;
    bool spec_disabled_ = false; // set once if the gate weight is quantized (unsupported), logged
    int n_expert_used_ = 0;
    float rms_eps_ = 1e-6f;
    std::vector<const ggml_tensor *> gate_w_;      // per-layer router weight (mmap-resident), or null
    std::vector<const ggml_tensor *> gate_s_;      // per-layer router per-channel scale, or null
    std::vector<int> next_moe_layer_;              // next bound MoE layer after il, or -1
    std::vector<std::vector<int32_t>> spec_pred_;  // predicted experts per layer, for recall scoring
    std::vector<float> spec_hidden_, spec_logits_; // scratch
    long long spec_pred_total_ = 0, spec_pred_hit_ = 0;
};

} // namespace bmoe
