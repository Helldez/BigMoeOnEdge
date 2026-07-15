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
//
// Streaming carries an optional third job, the route trace (set_trace): the same pass also
// asks for each layer's router-weight node and records which experts were routed, how the
// router weighted them, and whether they were already resident. It observes only — the ids
// handed to load_layer are the same traced or not — but it costs a barrier per weight node,
// so it stays off unless asked for. See bmoe/route_trace.h.
#pragma once

#include "bmoe/recipe.h"
#include "bmoe/route_trace.h"
#include "expert_stream_source.h"

#include <cstdint>
#include <unordered_set>
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

    void set_source(IExpertSource * src) { source_ = src; } // non-null → stream mode

    // Temporal prefetch depth K: while streaming layer l, hint the source to read ahead the
    // experts the previous token used at layers l+1..l+K. 0 (default) disables it.
    void set_prefetch_layers(int k) { prefetch_layers_ = k; }

    // ── route trace (diagnostics; see bmoe/route_trace.h) ────────────────────────────
    // When on, the hook additionally asks for each layer's router-weight node and records one
    // RouteTraceRow per routed expert. Rows buffer in RAM — the callback runs on a compute
    // thread mid-graph, so it must not do I/O — and the session drains them once llama_decode
    // has returned. Off by default: asking for the extra nodes costs a barrier per layer.
    void set_trace(bool on);

    // Frame the rows of one llama_decode. `base_pos` is the context position of the batch's
    // first token and `n_tokens` its length, so a prefill chunk's rows carry real per-token step
    // numbers — and so a layer that saw fewer tokens than the batch can still be placed (see
    // flush_pending).
    void begin_trace_batch(int base_pos, int n_tokens, int phase, int turn);
    void end_trace_batch(); // flush the last layer, which has no successor to trigger it

    std::vector<RouteTraceRow> & trace_rows() { return trace_rows_; }

private:
    bool on_eval(ggml_tensor * t, bool ask);

    // Row-building state for the layer whose topk we last saw. A layer's weights arrive in a
    // LATER callback than its ids, so its rows can only be built once the whole weight chain has
    // been offered — i.e. when the next layer's topk arrives, or when the batch ends.
    struct PendingLayer {
        int layer = -1;
        int nu = 0, nt = 0;
        std::vector<int32_t> ids;
        std::vector<float> weights;
        std::vector<uint8_t> residency;
    };
    void flush_pending();

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

    // Route trace. All of this is inert unless trace_on_.
    bool trace_on_ = false;
    int trace_base_pos_ = 0, trace_batch_n_ = 1, trace_phase_ = 0, trace_turn_ = 0;
    PendingLayer pending_;
    std::vector<RouteTraceRow> trace_rows_;
    std::unordered_set<int32_t> charged_; // per-flush scratch: experts already charged for a read
};

} // namespace bmoe
