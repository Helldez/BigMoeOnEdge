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
//
// And an optional fourth (set_predict_log): the expert-prediction probe, which asks for each
// layer's gate matmul so it can rank the NEXT layer's experts before that layer runs, and reports
// how often the ranking was right. It decides nothing — it exists to price a predictor before one
// is wired into the loading path. See bmoe/predict_stats.h.
#pragma once

#include "bmoe/recipe.h"
#include "bmoe/route_trace.h"
#include "bmoe/decode_trace.h"
#include "bmoe/predict_stats.h"
#include "expert_stream_source.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ggml_tensor;

namespace bmoe {

class RouterHook {
public:
    RouterHook(const MoeRecipe & recipe, int n_layer);
    ~RouterHook(); // joins the prediction worker, if one was started

    // Install as ctx_params.cb_eval / cb_eval_user_data before context creation.
    static bool c_eval(ggml_tensor * t, bool ask, void * user_data);

    void begin_capture(); // switch to capture; clears prior records
    void end_capture();   // stop recording (called after the warm-up decode)

    // After capture, the tensors found per (layer, projection). Entry.bound is false for
    // layers that produced no expert tensors (dense layers). file_off/nb2 are filled in
    // by the runtime from the gguf; here only .tensor is set.
    const std::vector<LayerExperts> & captured() const { return captured_; }
    std::vector<LayerExperts> & captured() { return captured_; }

    // After capture, the non-expert weight LEAVES seen in the graph, keyed by tensor name. These
    // are the persistent model weights the streamer leaves mmap-resident (embeddings, attention,
    // norms, lm_head); the --dense-weights anon policy rebinds them onto O_DIRECT buffers. The map
    // also holds graph inputs and KV tensors (same op-NONE leaf shape); the runtime filters it
    // against the gguf tensor set, which those do not belong to. Only .tensor is meaningful here.
    const std::unordered_map<std::string, ggml_tensor *> & captured_weights() const { return captured_weights_; }

    void set_source(IExpertSource * src) { source_ = src; } // non-null → stream mode

    // Temporal prefetch depth K: while streaming layer l, hint the source to read ahead the
    // experts the previous token used at layers l+1..l+K. 0 (default) disables it.
    void set_prefetch_layers(int k) { prefetch_layers_ = k; }

    // ── expert-prediction accuracy probe (diagnostics; see bmoe/predict_stats.h) ──────
    // Measures, without changing a byte of what gets loaded, how well the next layer's routing can
    // be guessed before that layer runs. Three predictors are scored against the routing the
    // router actually produced:
    //
    //   stale-gate  layer l's gate INPUT run through layer l+1's gate matrix. The residual stream
    //               moves little between layers, so the stale input ranks l+1's experts nearly as
    //               the real one will — a full layer earlier. Costs one GEMV per layer; needs no
    //               training and no model change.
    //   prev-token  what the PREVIOUS token routed at this layer: the predictor --prefetch already
    //               bets on, scored here so the two are comparable on the same run.
    //   fresh-gate  the layer's own matrix on its own gate input: the stale predictor with the
    //               staleness taken out, sharing its every line of code. A control, not a
    //               predictor — it must reproduce the routing the graph computes from those same
    //               two tensors, so anything it misses is this probe being wrong (a GEMV that
    //               disagrees with llama.cpp's, or a selection the architecture does not make by
    //               raw-logit ranking: an additive bias, group-limited routing). Read the other
    //               two against it: a fresh-gate below 100% caps what the stale one could show.
    //
    // Decode only, last token of the batch. Off by default: it isolates one extra node per MoE
    // layer and does a per-layer GEMV on the eval thread, so a probed run is not a benchmark run.
    void set_predict_log(bool on);

    // ── predictive prefetch (see MoeStreamConfig::predict_prefetch) ───────────────────
    // Act on the stale-gate prediction: hand the predicted next-layer experts to the source's
    // speculative read path, exactly as the temporal prefetch does with the previous token's ids.
    // Drop-aware by construction — with the drop policy armed, a predicted expert whose predicted
    // routing weight sits below the drop threshold is not speculated: if it misses, the policy
    // would discard it unread, so prefetching it buys nothing and costs the read. Independent of
    // set_predict_log (the probe adds scoring and the control on top of the same prediction).
    // spec_max: how many predicted misses a layer may speculate (0 = retention only). Retention of
    // predicted residents happens at every value — it costs zero bytes.
    void set_predict_prefetch(bool on, int spec_max = 2);

    // ── route trace (diagnostics; see bmoe/route_trace.h) ────────────────────────────
    // When on, the hook additionally asks for each layer's router-weight node and records one
    // RouteTraceRow per routed expert. Rows buffer in RAM — the callback runs on a compute
    // thread mid-graph, so it must not do I/O — and the session drains them once llama_decode
    // has returned. Off by default: asking for the extra nodes costs a barrier per layer.
    void set_trace(bool on);

    // ── cache-aware expert dropping (lossy; see MoeStreamConfig::drop_cold_frac) ──────
    // `frac` > 0 arms the policy: a routed expert that is a cache MISS and carries less than
    // frac × (1/n_expert_used) of the routing's weight is discarded — not read, weight zeroed,
    // its slot pointed at the routing's top-weighted expert so the matmul still reads memory
    // that is certainly resident. `renorm` rescales the survivors to preserve the routing's
    // total mass. Off (frac == 0) the hook behaves exactly as before, bit for bit.
    void set_drop_policy(float frac, bool renorm, bool in_prefill);

    // Which phase the batch being decoded belongs to (0 = prefill, 1 = decode). The drop policy
    // is decode-only unless armed for prefill, and unlike the traces it must know this on every
    // run, so it cannot ride on begin_trace_batch.
    void set_batch_phase(int phase) { batch_phase_ = phase; }

    long long experts_routed() const { return experts_routed_; }
    long long experts_dropped() const { return experts_dropped_; }

    // Prediction accuracy, aggregate and per layer (index = layer). Empty/zero unless the probe
    // was armed. `predict_unscored` counts routings the probe had to skip: the first token, whose
    // layers have not yet seen the next layer's gate matrix, plus any layer whose gate or hidden
    // state is a dtype the probe does not read. It is reported rather than folded into the
    // denominator — a skipped routing is not a wrong guess.
    const PredictorStats & predict_stale() const { return agg_stale_; }
    const PredictorStats & predict_stale2() const { return agg_stale2_; }
    const PredictorStats & predict_prev() const { return agg_prev_; }
    const PredictorStats & predict_self() const { return agg_self_; }
    const std::vector<PredictorStats> & predict_stale_by_layer() const { return ps_stale_; }
    const std::vector<PredictorStats> & predict_prev_by_layer() const { return ps_prev_; }
    const std::vector<PredictorStats> & predict_self_by_layer() const { return ps_self_; }
    long long predict_unscored() const { return predict_unscored_; }

    // ── compute trace (diagnostics; see bmoe/decode_trace.h) ────────────────────────
    // When on, the hook asks for EVERY node, which makes ggml compute and synchronize each one
    // alone — so the wall delta between consecutive callbacks is that node's real compute time,
    // and the major-fault delta across it is the flash re-read that time was actually spent on.
    // This is the only way to measure compute from outside llama.cpp, and it is expensive by
    // construction: a barrier per node, and no operator coalescing. Off by default; a traced run
    // is not a benchmark run. Independent of streaming — a dense baseline can be traced too.
    //
    // per_layer trades the per-op detail for fidelity: only the first node of each layer is
    // isolated (~n_layer barriers per token instead of thousands), coalescing and the async
    // expert prefetch survive, and each row aggregates one layer's segment (op "LAYER").
    void set_compute_trace(bool on, bool per_layer = false);

    // Frame the rows of one llama_decode, as begin_trace_batch does for the route trace. Rows are
    // stamped with `step`; a prefill chunk attributes its whole graph to the batch's last position,
    // since a node is computed once for the batch, not per token.
    void begin_compute_batch(int step, int phase, int turn);

    // Close the batch's final interval (layer-granularity only): the last layer's tail plus the
    // final norm and LM head have no successor boundary to observe them, so the session must call
    // this right after llama_decode returns — the row is charged the wall since the last boundary.
    void end_compute_batch();

    std::vector<ComputeTraceRow> & compute_rows() { return compute_rows_; }

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
        std::vector<uint8_t> dropped; // set by the drop policy; all zero when it is off
    };
    void flush_pending();
    bool drop_armed() const;
    void apply_drop(ggml_tensor * weights);
    void close_drop_layer();
    void ctrace_close_segment(int interval_layer, const char * tail_name);

    // Stored by value, not by reference: the caller often constructs us from a temporary
    // (a `cond ? *ptr : MoeRecipe{}` ternary yields a prvalue even when ptr is non-null),
    // which would leave a reference member dangling. The struct is just a handful of string
    // literal pointers, so copying is cheap and keeps the suffixes valid for our lifetime.
    MoeRecipe recipe_;
    int n_layer_ = 0;
    bool capturing_ = false;
    IExpertSource * source_ = nullptr; // non-null → stream mode
    std::vector<LayerExperts> captured_;
    std::unordered_map<std::string, ggml_tensor *> captured_weights_; // non-expert weight leaves (see captured_weights)
    std::vector<int32_t> gathered_;                                   // reused scratch for stream-mode id gather

    // Temporal prefetch: K, and the previous token's routed experts per layer (last-token row
    // during prefill). Empty when prefetch is off or a layer has not been seen yet.
    int prefetch_layers_ = 0;
    std::vector<std::vector<int32_t>> prev_ids_;

    // Prediction (probe and/or prefetch). All of this is inert unless one of the two is on.
    //
    // gate_w_ is learned from the graph rather than looked up by name: the gate matmul's first
    // source IS the router matrix, whatever the architecture calls its tensor. It is a weight LEAF,
    // so unlike a graph intermediate the pointer stays valid across graphs — which is what lets
    // layer l predict for layer l+1 using a matrix layer l+1 has not reached yet this token.
    bool predict_log_ = false;
    bool predict_prefetch_ = false;
    std::vector<ggml_tensor *> gate_w_;
    std::vector<std::vector<int32_t>> pred_stale_;  // per layer, ranked one layer early
    std::vector<std::vector<int32_t>> pred_stale2_; // per layer, ranked TWO layers early (probe only)
    std::vector<std::vector<int32_t>> pred_self_;   // per layer, ranked from its own logits
    std::vector<PredictorStats> ps_stale_, ps_prev_, ps_self_;
    PredictorStats agg_stale_, agg_stale2_, agg_prev_, agg_self_;
    long long predict_unscored_ = 0;
    std::vector<float> pred_scores_; // scratch: one score per expert
    std::vector<float> pred_row_;    // scratch: one activation row, as float
    // How wide a routing actually is, learned from the last topk node seen. The prefetch needs it
    // BEFORE the predicted layer's topk exists, and reading it off the graph (rather than config)
    // keeps an --n-expert-used override honest. 0 until the first routing of a run has been seen,
    // during which the prefetch stays silent.
    int nu_hint_ = 0;
    std::vector<int32_t> spec_ids_; // scratch: the filtered prediction handed to prefetch()
    std::vector<uint8_t> pred_res_; // scratch: residency of the prediction being filtered
    int pred_spec_max_ = 2;         // speculated predicted misses per layer (0 = retention only)

    // ── the prefetch's own prediction path (no barrier, GEMV off the eval thread) ─────
    //
    // The probe pays for its reading with an isolated node per layer; the prefetch cannot afford
    // that (the barrier alone measured ~20 ms/token) so it reads WITHOUT one: the gate matmul's
    // source pointers are stashed in the ask pass — which sees every node isolated or not — and
    // the gate-input row is copied at the topk callback, by which point the graph has certainly
    // computed it. What is NOT certain is that ggml's memory planner has not reused the buffer in
    // between; the watchdog below is what turns that risk into a measured quantity.
    //
    // The GEMV itself runs on a dedicated worker, and the horizon is TWO layers: h at layer l
    // ranks layer l+2. One layer ahead cannot work off-thread — the only eval callbacks between a
    // layer's gate and its own load are microseconds apart, so the result would always miss its
    // deadline. At l+2 the worker has a whole layer (~ms) for a ~0.5M-MAC job, and the result is
    // collected at layer l+1's load (predict_after_load), inheriting the same post-load issue
    // window the temporal prefetch uses. The price is one more layer of staleness; the probe's
    // stale-2 column is what says how much accuracy that costs on a given model. A result that is
    // not ready in time is skipped, never waited for.
    std::vector<ggml_tensor *> h_t_; // per layer, the gate matmul's input, stashed in the ask pass

    struct PredictJob {
        uint64_t seq = 0;
        int nl = 0; // layer the prediction is FOR
        int nu = 0;
        float drop_frac = 0.0f;
        int spec_max = 2;
        const ggml_tensor * gate = nullptr; // layer nl's gate matrix, snapshotted with the job
        std::vector<float> row;             // the gate-input row, copied on the eval thread
        std::vector<uint8_t> resident;      // residency bitmap of layer nl, snapshotted on the eval thread
    };
    struct PredictResult {
        uint64_t seq = 0;
        int nl = -1;
        std::vector<int32_t> spec; // predicted misses to speculate, confidence-ordered, capped
        std::vector<int32_t> keep; // predicted residents to retain (LRU-protect)
        bool ready = false;
    };
    std::thread pred_worker_;
    std::mutex pred_mtx_;
    std::condition_variable pred_cv_;
    PredictJob pred_job_;
    PredictResult pred_result_;
    bool pred_job_pending_ = false;
    bool pred_stop_ = false;
    uint64_t pred_seq_ = 0;

    // Watchdog for the barrier-less read. Every wd_interval routings the freshly-stashed row is
    // run through the layer's OWN gate and checked against the ids the router actually chose —
    // the same zero-staleness control the probe uses, sampled instead of continuous. If it drifts
    // below the threshold the buffer is being reused (or the architecture does not select by
    // raw-logit ranking) and the prefetch disarms itself, once, out loud. Without this, a planner
    // change in a future llama.cpp bump would turn the predictor into a silent noise generator.
    long long wd_slots_ = 0, wd_hits_ = 0, wd_rout_ = 0;
    bool wd_tripped_ = false;
    static constexpr int wd_interval = 512;         // routings between samples (~13 tokens at 40 layers)
    static constexpr double wd_min_frac = 0.98;     // below this, the read is not trustworthy
    static constexpr long long wd_min_slots = 2048; // do not judge before this many slots

    bool predict_on() const { return predict_log_ || predict_prefetch_; }
    void predict_reset();
    void predict_worker_stop();
    void predict_worker_main();

    // Rank the top `k` of `scores` into `out`, and score a prediction against a routing.
    static void rank_top_k(const std::vector<float> & scores, int k, std::vector<int32_t> & out);
    static void score_prediction(
        const std::vector<int32_t> & pred, const int32_t * actual, int k, PredictorStats & layer, PredictorStats & agg);
    // From a full score vector: the top-nu ranking, softmax-filtered against the drop threshold,
    // split into misses to speculate (capped) and residents to retain.
    static void build_spec_lists(const std::vector<float> & scores,
                                 int nu,
                                 float drop_frac,
                                 int spec_max,
                                 const std::vector<uint8_t> & resident,
                                 std::vector<int32_t> & spec,
                                 std::vector<int32_t> & keep);
    void predict_at_logits(ggml_tensor * logits, int il);
    void score_layer(int il, const int32_t * actual, int nu);
    void predict_at_topk(ggml_tensor * t, int il, int nu, int nt); // watchdog + job submission
    void predict_after_load(int il);                               // collect + issue once il's load is behind us

    // The probe's maximum prediction width. A routing wider than this would be scored against a
    // truncated prediction and read as a miss it never was, so the probe declines it instead.
    static constexpr int predict_max_k = 32;

    // Cache-aware dropping. Inert unless drop_frac_ > 0.
    //
    // The decision needs the FINAL router weights, and those are produced several nodes after the
    // topk that opens the layer — so load_layer() is postponed from the topk node to the terminal
    // node of the layer's weight chain, and the ids/weights are edited there, before the expert
    // matmul consumes either. Which node is terminal depends on the model's gating (norm, softmax,
    // scaled, or none of them), so it is LEARNED from the graph rather than tabulated per
    // architecture: term_node_[il] fills in on the first graph, and until it does the layer loads
    // at its topk node undropped, exactly as with the policy off. That costs the first token of a
    // run its dropping and nothing else.
    float drop_frac_ = 0.0f;
    bool drop_renorm_ = true;
    bool drop_prefill_ = false;
    int batch_phase_ = 1; // 0 prefill, 1 decode
    long long experts_routed_ = 0, experts_dropped_ = 0;

    struct PendingDrop {
        int layer = -1;
        int nu = 0, nt = 0;
        ggml_tensor * ids = nullptr; // the topk view, rewritten in place for dropped slots
        bool deferred = false;       // true when load_layer is waiting for the terminal weight node
    };
    PendingDrop drop_;
    std::vector<std::string> term_node_; // per layer, "" until learned
    std::string chain_last_;             // last weight node seen for drop_.layer while its chain runs
    std::vector<int32_t> drop_ids_;      // this layer's routed ids, kept across the deferral
    std::vector<float> drop_w_;          // scratch: the final weights
    std::vector<uint8_t> drop_res_;      // scratch: residency of each routed id
    std::vector<uint8_t> drop_mask_;     // scratch: which slots this layer dropped

    // Route trace. All of this is inert unless trace_on_.
    bool trace_on_ = false;
    int trace_base_pos_ = 0, trace_batch_n_ = 1, trace_phase_ = 0, trace_turn_ = 0;
    PendingLayer pending_;
    std::vector<RouteTraceRow> trace_rows_;
    std::unordered_set<int32_t> charged_; // per-flush scratch: experts already charged for a read

    // Compute trace. All of this is inert unless ctrace_on_. `ctrace_mark_` is the previous
    // isolation boundary: the node reported by the next callback is charged the wall since it.
    // Layer granularity keeps two cursors because ask and observe see different node streams:
    // ask_layer_ decides which nodes to isolate (every node is asked), obs_layer_ names the
    // segment an observed interval belongs to (only isolated nodes are observed). -1 = before
    // layer 0, i.e. the "pre" segment.
    bool ctrace_on_ = false;
    bool ctrace_layers_ = false;
    int ctrace_ask_layer_ = -1, ctrace_obs_layer_ = -1;
    int ctrace_step_ = 0, ctrace_phase_ = 0, ctrace_turn_ = 0;
    int ctrace_seq_ = 0;
    std::chrono::steady_clock::time_point ctrace_mark_;
    uint64_t ctrace_faults_ = 0;
    std::vector<ComputeTraceRow> compute_rows_;
};

} // namespace bmoe
