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

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

struct ggml_tensor;

namespace bmoe {

class RouterHook {
public:
    RouterHook(const MoeRecipe & recipe, int n_layer);
    ~RouterHook();

    // Install as ctx_params.cb_eval / cb_eval_user_data before context creation.
    static bool c_eval(ggml_tensor * t, bool ask, void * user_data);

    void begin_capture(); // switch to capture; clears prior records
    void end_capture();   // stop recording (called after the warm-up decode)

    // After capture, the tensors found per (layer, projection). Entry.bound is false for
    // layers that produced no expert tensors (dense layers). file_off/nb2 are filled in
    // by the runtime from the gguf; here only .tensor is set.
    const std::vector<LayerExperts> & captured() const { return captured_; }
    std::vector<LayerExperts> & captured() { return captured_; }

    void set_source(IExpertSource * src); // enter stream mode (starts the prediction worker)

    // Temporal prefetch depth K: while streaming layer l, hint the source to read ahead the
    // experts the previous token used at layers l+1..l+K. 0 (default) disables it.
    void set_prefetch_layers(int k) { prefetch_layers_ = k; }

    // Speculative gating: predict the next MoE layer's experts by running its router on the
    // current layer's hidden state, and prefetch them. Needs n_expert_used (top-k) and the rms
    // epsilon (for architectures whose router normalises its input). Call before begin_capture()
    // so the gate weights are harvested. Only the predicted ids feed prefetch, so a wrong
    // prediction wastes a read but never changes output. When sync is true the prediction runs
    // inline on the eval thread (deterministic ordering for the byte-identity gates); otherwise it
    // is dispatched to a dedicated worker thread so it does not inflate the eval-thread decode time.
    void set_spec_gate(bool on, int n_expert_used, float rms_eps, bool sync);

    // Speculative-gating prediction accuracy (cumulative): how many predicted experts a later
    // routing actually used, over how many predicted. Read by the runtime for telemetry.
    long long spec_pred_total() const { return spec_pred_total_; }
    long long spec_pred_hit() const { return spec_pred_hit_; }

private:
    bool on_eval(ggml_tensor * t, bool ask);

    // Speculative-gating prediction, split across two threads so the eval thread only pays for the
    // hidden-state snapshot (done inside the router-input barrier):
    //   spec_request    — eval thread: resolve the target layer, snapshot the router-input hidden
    //                     state, and dispatch it (inline when spec_sync_, else to the worker).
    //   spec_compute    — worker (or inline in sync mode): router pre-transform + logits + top-k.
    //   spec_drain_mailbox — eval thread: enqueue the worker's finished prediction into the
    //                        streamer's prefetch queue. The prefetch/LRU stay eval-thread-only.
    void spec_request(const ggml_tensor * node, int il);
    void spec_compute(int target, const std::vector<float> & hidden, std::vector<int32_t> & out) const;
    void spec_drain_mailbox();
    void spec_worker_loop();
    void spec_stop_worker();

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
    bool spec_sync_ = false;     // predict inline on the eval thread (test/deterministic path)
    bool spec_disabled_ = false; // set once if the gate weight is quantized (unsupported), logged
    int n_expert_used_ = 0;
    float rms_eps_ = 1e-6f;
    std::vector<const ggml_tensor *> gate_w_;     // per-layer router weight (mmap-resident), or null
    std::vector<const ggml_tensor *> gate_s_;     // per-layer router per-channel scale, or null
    std::vector<int> next_moe_layer_;             // next bound MoE layer after il, or -1
    std::vector<std::vector<int32_t>> spec_pred_; // predicted experts per layer, for recall scoring
    long long spec_pred_total_ = 0, spec_pred_hit_ = 0;

    // Prediction worker. The eval thread posts a latest-wins request (hidden-state snapshot + target
    // layer); the worker computes the top-k and posts it back in a single-slot mailbox that the eval
    // thread drains on a later callback. Everything the worker reads (gate_w_/gate_s_/recipe_/
    // next_moe_layer_/n_expert_used_/rms_eps_) is immutable after end_capture(), so no lock guards it;
    // pred_mtx_ guards only the request/result slots. Started in set_source(), joined in the dtor.
    std::thread pred_worker_;
    std::mutex pred_mtx_;
    std::condition_variable pred_cv_;
    bool pred_stop_ = false;
    bool pred_req_pending_ = false;
    int pred_req_target_ = -1;
    std::vector<float> pred_req_hidden_;
    bool pred_res_ready_ = false;
    int pred_res_target_ = -1;
    std::vector<int32_t> pred_res_ids_;
};

} // namespace bmoe
