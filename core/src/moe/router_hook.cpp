#include "router_hook.h"

#include "ggml.h"
#include "../io/platform_io.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace bmoe {

RouterHook::RouterHook(const MoeRecipe & recipe, int n_layer) : recipe_(recipe), n_layer_(n_layer) {
    const int n = n_layer_ > 0 ? n_layer_ : 0;
    captured_.assign(n, LayerExperts{});
    prev_ids_.assign(n, std::vector<int32_t>{});
}

// Match a tensor name of the form "blk.<il>.<suffix>.weight" against the recipe's expert
// tensor suffixes. Returns the slot index (its position in recipe.exps_suffix) and layer,
// or -1. The exact ".weight" tail comparison is load-bearing: a companion tensor like
// "ffn_down_exps.scale" prefix-matches its suffix but fails the tail strcmp, so per-expert
// scales — and every other non-".weight" tensor — are left mmap-resident, not streamed.
static int match_expert(const char * name, const MoeRecipe & r, int & il_out) {
    int il = -1;
    int consumed = 0;
    if (std::sscanf(name, "blk.%d.%n", &il, &consumed) != 1 || il < 0) return -1;
    const char * rest = name + consumed; // "<suffix>.weight"
    for (int p = 0; p < MoeRecipe::max_exps; ++p) {
        const char * suffix = r.exps_suffix[p];
        if (!suffix) continue; // unused slot (fewer than max_exps expert tensors)
        const size_t sl = std::strlen(suffix);
        if (std::strncmp(rest, suffix, sl) == 0 && std::strcmp(rest + sl, ".weight") == 0) {
            il_out = il;
            return p;
        }
    }
    return -1;
}

// Match the router-weight nodes build_moe_ffn emits per layer. The chain is ffn_moe_weights →
// (_softmax | _norm) → (_scaled), each an optional refinement of the previous, so the LAST one
// offered for a layer carries the weight actually applied to that layer's expert outputs.
// Last-wins is what keeps this architecture-independent: no table of which gating each model
// uses. The '-' check is load-bearing — it rejects "ffn_moe_weights_sum-3", which prefix-matches
// "ffn_moe_weights" but is a row sum, not a weight.
static bool match_weights(const char * name, int & il_out) {
    static const char * const kNodes[] = {"ffn_moe_weights", "ffn_moe_weights_softmax", "ffn_moe_weights_norm",
                                          "ffn_moe_weights_scaled"};
    for (const char * n : kNodes) {
        const size_t l = std::strlen(n);
        if (std::strncmp(name, n, l) != 0 || name[l] != '-') continue;
        int il = -1;
        if (std::sscanf(name + l + 1, "%d", &il) == 1 && il >= 0) {
            il_out = il;
            return true;
        }
    }
    return false;
}

// Gather the top-k router weights for every token of the batch. The node is [1, nu, nt] as built
// (token j at nb[2], slot k at nb[1]) — except the norm variant, whose callback fires on the
// pre-reshape 2-D [nu, nt] (token j at nb[1], slot k at nb[0]). ne[0] == 1 tells the two apart.
// As with the topk ids, these are views: only the strides say where a token's row really starts.
// Where token j's slot k lives inside a weight node. Single source of truth for the layout: the
// reader below and the drop policy's writer must agree, or the policy would zero another token's
// slot. Returns a mutable pointer; the gather takes a const tensor and only reads through it.
static float * weight_at(const ggml_tensor * t, int j, int k) {
    const bool three_d = t->ne[0] == 1;
    const size_t tok_nb = three_d ? t->nb[2] : t->nb[1];
    const size_t slot_nb = three_d ? t->nb[1] : t->nb[0];
    return (float *) ((char *) t->data + (size_t) j * tok_nb + (size_t) k * slot_nb);
}

static void gather_weights(const ggml_tensor * t, int nu, int nt, std::vector<float> & out) {
    out.assign((size_t) nu * nt, 0.0f);
    for (int j = 0; j < nt; ++j)
        for (int k = 0; k < nu; ++k)
            out[(size_t) j * nu + k] = *weight_at(t, j, k);
}

// Same, for the selected-expert ids. The node is a VIEW of the full argsort, so the row stride is
// nb[1] (n_expert * 4), not n_expert_used * 4 — see the gather at the topk node.
static int32_t * id_at(ggml_tensor * t, int j, int k) {
    return (int32_t *) ((char *) t->data + (size_t) j * t->nb[1] + (size_t) k * t->nb[0]);
}

void RouterHook::begin_capture() {
    capturing_ = true;
    for (auto & L : captured_)
        L = LayerExperts{};
    captured_weights_.clear();
}
void RouterHook::end_capture() {
    capturing_ = false;
}

void RouterHook::set_drop_policy(float frac, bool renorm, bool in_prefill) {
    drop_frac_ = frac > 0.0f ? frac : 0.0f;
    drop_renorm_ = renorm;
    drop_prefill_ = in_prefill;
    term_node_.assign(n_layer_ > 0 ? n_layer_ : 0, std::string{});
    drop_ = PendingDrop{};
    chain_last_.clear();
    experts_routed_ = experts_dropped_ = 0;
}

// Is dropping live for the batch being decoded? Needs a source to ask about residency, a non-zero
// threshold, and — unless armed for prefill — a decode batch: with a cold cache the same threshold
// discards several times the weight mass for a phase that is not I/O-bound anyway.
bool RouterHook::drop_armed() const {
    return drop_frac_ > 0.0f && source_ != nullptr && (batch_phase_ == 1 || drop_prefill_);
}

// Apply the policy to the layer held in drop_, then load only what survives.
//
// `wt` is the terminal node of the weight chain: the weights as the expert matmul will apply them.
// Both edits happen here, before any node consumes them:
//   - the dropped slot's weight is zeroed (and, with renorm, the survivors are scaled back up so
//     the routing keeps the total mass it had);
//   - the dropped slot's ID is repointed at the routing's top-weighted expert. That second edit is
//     not cosmetic. An expert we decline to read may sit in a reserved-but-uncommitted slot, and
//     mul_mat_id would still touch it; pointing the slot at an expert that is certainly resident
//     makes the kernel read valid memory and multiply it by exactly zero. It costs a duplicate
//     matmul, which is the right trade on a decode bound by flash rather than arithmetic.
// The top-weighted expert is never dropped, so a routing always keeps at least one live expert
// whatever the threshold — the guarantee does not rest on frac <= 1 alone.
void RouterHook::apply_drop(ggml_tensor * wt) {
    PendingDrop & D = drop_;
    const int nu = D.nu, nt = D.nt;
    gather_weights(wt, nu, nt, drop_w_);

    // Classify against the cache BEFORE anything is loaded; settle landed prefetches first, or an
    // expert a prefetch correctly guessed would look like a miss and be dropped for nothing.
    source_->settle_spec();
    drop_res_.assign(drop_ids_.size(), (uint8_t) 0);
    source_->query_residency(D.layer, drop_ids_.data(), (int) drop_ids_.size(), drop_res_.data());

    const float thr = drop_frac_ / (float) nu; // frac of the uniform share each of k experts would get
    const bool tracing = trace_on_ && pending_.layer == D.layer;
    drop_mask_.assign((size_t) nu * nt, (uint8_t) 0);

    for (int j = 0; j < nt; ++j) {
        const size_t row = (size_t) j * nu;
        int best = 0;
        float total = 0.0f;
        for (int k = 0; k < nu; ++k) {
            total += drop_w_[row + k];
            if (drop_w_[row + k] > drop_w_[row + best]) best = k;
        }
        const int32_t best_id = drop_ids_[row + best];

        float kept = 0.0f;
        int n_dropped = 0;
        for (int k = 0; k < nu; ++k) {
            const size_t idx = row + k;
            const bool drop = k != best && drop_res_[idx] == route_miss && drop_w_[idx] < thr;
            if (!drop) {
                kept += drop_w_[idx];
                continue;
            }
            *weight_at(wt, j, k) = 0.0f;
            *id_at(D.ids, j, k) = best_id;
            drop_ids_[idx] = best_id;
            drop_mask_[idx] = 1;
            ++n_dropped;
        }
        experts_dropped_ += n_dropped;

        // Restore the routing's total mass. Without this the layer's expert output is scaled down
        // by whatever was discarded, which perturbs the residual stream in a direction the model
        // never sees in training — a systematic shrink, unlike the one missing contribution.
        if (drop_renorm_ && n_dropped > 0 && kept > 0.0f) {
            const float g = total / kept;
            for (int k = 0; k < nu; ++k)
                if (!drop_mask_[row + k]) *weight_at(wt, j, k) *= g;
        }
    }

    if (tracing) pending_.dropped = drop_mask_;
    source_->load_layer(D.layer, drop_ids_.data(), (int) drop_ids_.size());
    D.deferred = false;
}

// Finish with the layer whose topk we last saw: record which node ended its weight chain, so the
// next graph can decide there, and make sure nothing was left waiting on a node that never came.
void RouterHook::close_drop_layer() {
    PendingDrop & D = drop_;
    if (D.layer < 0) return;
    if (D.layer < (int) term_node_.size() && term_node_[D.layer].empty() && !chain_last_.empty())
        term_node_[D.layer] = chain_last_;
    if (D.deferred && source_) {
        // The node we learned as terminal did not appear this time, so the deferral was never
        // honoured and this layer's matmul has already run against slots nothing loaded. Load the
        // routing now to keep the cache's accounting straight, and — more importantly — FORGET the
        // terminal node, so the next graph re-learns it and loads at the topk node meanwhile.
        // Deferring again on the same stale guess would repeat the fault every single token; one
        // bad layer in one token is recoverable, a standing bet against a graph that moved is not.
        source_->load_layer(D.layer, drop_ids_.data(), (int) drop_ids_.size());
        if (D.layer < (int) term_node_.size()) term_node_[D.layer].clear();
        D.deferred = false;
    }
    D.layer = -1;
}

// ── expert-prediction accuracy probe ────────────────────────────────────────────────────
//
// Observes only: it reads the gate matrix and the gate input, ranks experts from them, and
// compares the ranking with what the router went on to select. Nothing it computes reaches
// load_layer, the cache, or the graph, so a probed run routes and reads exactly as an unprobed
// one — it is only slower.

void RouterHook::set_predict_log(bool on) {
    predict_log_ = on;
    const int n = n_layer_ > 0 ? n_layer_ : 0;
    gate_w_.assign(n, nullptr);
    pred_stale_.assign(n, std::vector<int32_t>{});
    pred_self_.assign(n, std::vector<int32_t>{});
    ps_stale_.assign(n, PredictorStats{});
    ps_prev_.assign(n, PredictorStats{});
    ps_self_.assign(n, PredictorStats{});
    agg_stale_ = agg_prev_ = agg_self_ = PredictorStats{};
    predict_unscored_ = 0;
}

// Copy one token's row out of a 2-D activation as float, honouring the strides (the row may be a
// view). Returns false for a dtype the probe does not read, which is how an unsupported model
// declines the measurement instead of reporting a wrong one.
static bool row_to_float(const ggml_tensor * t, int j, std::vector<float> & out) {
    const int64_t n = t->ne[0];
    if (n <= 0) return false;
    const char * row = (const char *) t->data + (size_t) j * t->nb[1];
    out.assign((size_t) n, 0.0f);
    if (t->type == GGML_TYPE_F32) {
        for (int64_t d = 0; d < n; ++d)
            out[(size_t) d] = *(const float *) (row + (size_t) d * t->nb[0]);
        return true;
    }
    if (t->type == GGML_TYPE_F16) {
        for (int64_t d = 0; d < n; ++d)
            out[(size_t) d] = ggml_fp16_to_fp32(*(const ggml_fp16_t *) (row + (size_t) d * t->nb[0]));
        return true;
    }
    return false;
}

// The router's own arithmetic: one score per expert, from a gate matrix [n_embd, n_expert] and a
// gate input row. This is the whole prediction — the same GEMV the graph will do a layer later,
// done early on an input that has not finished changing.
static bool gate_scores(const ggml_tensor * w, const std::vector<float> & h, std::vector<float> & out) {
    const int64_t nd = w->ne[0], ne = w->ne[1];
    if (nd != (int64_t) h.size() || ne <= 0) return false;
    if (w->type != GGML_TYPE_F32 && w->type != GGML_TYPE_F16) return false;
    out.assign((size_t) ne, 0.0f);
    for (int64_t e = 0; e < ne; ++e) {
        const char * row = (const char *) w->data + (size_t) e * w->nb[1];
        float acc = 0.0f;
        if (w->type == GGML_TYPE_F32) {
            const float * p = (const float *) row;
            for (int64_t d = 0; d < nd; ++d)
                acc += p[d] * h[(size_t) d];
        } else {
            const ggml_fp16_t * p = (const ggml_fp16_t *) row;
            for (int64_t d = 0; d < nd; ++d)
                acc += ggml_fp16_to_fp32(p[d]) * h[(size_t) d];
        }
        out[(size_t) e] = acc;
    }
    return true;
}

// Ranking on RAW scores is exact wherever the gating function is monotonic — softmax, sigmoid and
// sqrt-softplus all are, so the order of the logits is the order of the probabilities the router
// sorts. It is NOT exact where the architecture adds a per-expert selection bias or masks whole
// expert groups before the argsort; there the ranking here is an approximation, and the fresh-gate
// control is what reveals it (it will sit below 100%) instead of the loss being blamed on staleness.
void RouterHook::rank_top_k(const std::vector<float> & scores, int k, std::vector<int32_t> & out) {
    const int n = (int) scores.size();
    const int want = k < n ? k : n;
    out.clear();
    if (want <= 0) return;
    out.reserve((size_t) want);
    // Partial selection rather than a sort: `want` is at most predict_max_k and n at most a few
    // hundred, so this is cheaper than ordering the tail nobody reads, and it needs no scratch.
    for (int i = 0; i < want; ++i) {
        int best = -1;
        for (int e = 0; e < n; ++e) {
            bool used = false;
            for (int32_t taken : out)
                if (taken == e) {
                    used = true;
                    break;
                }
            if (used) continue;
            if (best < 0 || scores[(size_t) e] > scores[(size_t) best]) best = e; // ties to the lower index
        }
        if (best < 0) break;
        out.push_back((int32_t) best);
    }
}

// Score one prediction against one routing. Only the prediction's first k entries count: a
// prefetch would fetch k experts, so crediting a hit found deeper in the ranking would measure a
// predictor nobody could build.
void RouterHook::score_prediction(
    const std::vector<int32_t> & pred, const int32_t * actual, int k, PredictorStats & layer, PredictorStats & agg) {
    const int np = (int) pred.size() < k ? (int) pred.size() : k;
    int hits = 0;
    for (int i = 0; i < k; ++i)
        for (int p = 0; p < np; ++p)
            if (pred[(size_t) p] == actual[i]) {
                ++hits;
                break;
            }
    const long long exact = hits == k ? 1 : 0;
    layer.rows += 1;
    layer.slots += k;
    layer.hits += hits;
    layer.exact += exact;
    agg.rows += 1;
    agg.slots += k;
    agg.hits += hits;
    agg.exact += exact;
}

// At the gate matmul of layer il: learn its router matrix, rank its own logits (the control), and
// predict layer il+1 from the input this layer is about to consume.
void RouterHook::predict_at_logits(ggml_tensor * t, int il) {
    if (il < 0 || il >= n_layer_) return;

    // The gate matmul's sources are the router matrix and the gate input. Taking the matrix from
    // the graph instead of looking it up by tensor name is what keeps the probe architecture-neutral
    // — and it is a weight LEAF, so unlike a graph intermediate the pointer stays valid into the
    // layers this token has not reached yet, which is exactly what predicting forward needs.
    ggml_tensor * w = t->src[0];
    ggml_tensor * h = t->src[1];
    if (w && w->op == GGML_OP_NONE && w->data && ggml_is_contiguous(w)) gate_w_[il] = w;

    const int nt = (int) t->ne[1];
    if (nt <= 0) return;
    const int j = nt - 1; // the batch's last token, the one prev_ids_ also records
    const int nl = il + 1;

    pred_self_[il].clear();
    if (nl < n_layer_) pred_stale_[nl].clear();

    // Both rankings below run the same code over the same gate input; only the matrix differs. That
    // is what makes the control a control — it shares the row read, the GEMV and the ranking with
    // the prediction under test, so a bug in any of them surfaces as a control below 100% instead
    // of quietly depressing the number the experiment is about.
    if (!h || !h->data || !row_to_float(h, j, pred_row_)) return;

    // Fresh-gate control: this layer's own matrix on its own input, with no staleness at all. It has
    // to reproduce the routing the graph is about to compute from those very two tensors, so it is
    // also the check that this GEMV agrees with llama.cpp's.
    if (gate_w_[il] && gate_scores(gate_w_[il], pred_row_, pred_scores_))
        rank_top_k(pred_scores_, predict_max_k, pred_self_[il]);

    // Stale-gate: the NEXT layer's matrix on this layer's input — the prediction under test. Silent
    // on the first token of a run, whose next layer has not been seen yet; score_layer counts those.
    if (nl < n_layer_ && gate_w_[nl] && gate_scores(gate_w_[nl], pred_row_, pred_scores_))
        rank_top_k(pred_scores_, predict_max_k, pred_stale_[nl]);
}

// At the topk of layer il: compare every predictor with what the router actually chose. Must run
// before prev_ids_[il] is overwritten, or the previous-token predictor would be handed the answer.
void RouterHook::score_layer(int il, const int32_t * actual, int nu) {
    if (nu <= 0 || nu > predict_max_k) { // a routing wider than the probe ranks; declined, not missed
        ++predict_unscored_;
        return;
    }
    if (pred_stale_[il].empty())
        ++predict_unscored_;
    else
        score_prediction(pred_stale_[il], actual, nu, ps_stale_[il], agg_stale_);
    if (!pred_self_[il].empty()) score_prediction(pred_self_[il], actual, nu, ps_self_[il], agg_self_);
    if (!prev_ids_[il].empty()) score_prediction(prev_ids_[il], actual, nu, ps_prev_[il], agg_prev_);
    pred_stale_[il].clear();
    pred_self_[il].clear();
}

void RouterHook::set_trace(bool on) {
    trace_on_ = on;
    pending_ = PendingLayer{};
    trace_rows_.clear();
}

void RouterHook::begin_trace_batch(int base_pos, int n_tokens, int phase, int turn) {
    trace_base_pos_ = base_pos;
    trace_batch_n_ = n_tokens > 0 ? n_tokens : 1;
    trace_phase_ = phase;
    trace_turn_ = turn;
    pending_ = PendingLayer{};
    trace_rows_.clear();
}

void RouterHook::end_trace_batch() {
    flush_pending();
}

// Turn the pending layer's (ids, weights, residency) into one row per routed expert.
void RouterHook::flush_pending() {
    PendingLayer & P = pending_;
    if (P.layer < 0 || P.nu <= 0 || P.nt <= 0) return;

    // Which context position is this layer's token j? Normally the batch's j-th. But before the
    // LAST layer's FFN, llama.cpp gathers only the tokens whose logits were asked for
    // (inp_out_ids; see e.g. models/qwen3moe.cpp "il == n_layer - 1"), so that layer routes
    // fewer tokens than the batch — during prefill, one. Our decode loops ask for logits on the
    // final token only, so a single-token row set is that final token, not the batch's first.
    // Attributing it to base_pos would silently misplace the last layer's whole prefill.
    // Anything else (n_outputs > 1) we cannot map, and say so with a negative step rather than
    // guess; the CLI's greedy loops never produce it.
    const int base = trace_base_pos_, span = trace_batch_n_, nt = P.nt;
    auto position_of = [base, span, nt](int j) -> int {
        if (nt == span) return base + j;
        if (nt == 1) return base + span - 1;
        return -1;
    };

    // A layer with no weight node seen (fused graph, unknown gating) reports NaN rather than 0 —
    // see route_trace.h. Anything else means the gather and the ids disagree, so distrust both.
    const bool have_w = P.weights.size() == (size_t) P.nu * P.nt;
    const uint64_t ebytes = source_ ? source_->expert_bytes(P.layer) : 0;

    // A missing expert is read ONCE per decode however many of the batch's tokens route it
    // (load_layer dedups), so only its first row carries the byte cost. Residency stays per-row:
    // every one of those routings did face a cold cache.
    charged_.clear();
    for (int j = 0; j < P.nt; ++j) {
        for (int k = 0; k < P.nu; ++k) {
            const size_t idx = (size_t) j * P.nu + k;
            RouteTraceRow r;
            r.turn = trace_turn_;
            r.phase = trace_phase_;
            r.step = position_of(j);
            r.layer = P.layer;
            r.slot = k;
            r.expert = P.ids[idx];
            r.weight = have_w ? P.weights[idx] : std::numeric_limits<float>::quiet_NaN();
            r.residency = idx < P.residency.size() ? P.residency[idx] : (uint8_t) 0;
            r.dropped = idx < P.dropped.size() ? P.dropped[idx] : (uint8_t) 0;
            // A dropped routing is never read, so it is neither charged nor allowed to claim the
            // charge for its expert: if another token of the batch routes the same expert and keeps
            // it, that routing pays the read.
            if (!r.dropped && r.residency == route_miss && charged_.insert(r.expert).second) r.expert_bytes = ebytes;
            trace_rows_.push_back(r);
        }
    }
    P.layer = -1;
    P.nu = P.nt = 0;
}

bool RouterHook::c_eval(ggml_tensor * t, bool ask, void * user_data) {
    return static_cast<RouterHook *>(user_data)->on_eval(t, ask);
}

// Layer id from a node name. llama.cpp suffixes per-layer nodes with "-<il>"; anything else
// (embeddings, the output head, the KQ mask) belongs to no layer and reports -1. Kept generic on
// purpose: the trace must not carry a table of which node names a given architecture emits.
static int node_layer(const char * name) {
    const char * dash = std::strrchr(name, '-');
    if (!dash || !dash[1]) return -1;
    for (const char * p = dash + 1; *p; ++p)
        if (*p < '0' || *p > '9') return -1;
    return std::atoi(dash + 1);
}

void RouterHook::set_compute_trace(bool on, bool per_layer) {
    ctrace_on_ = on;
    ctrace_layers_ = per_layer;
    compute_rows_.clear();
}

void RouterHook::begin_compute_batch(int step, int phase, int turn) {
    ctrace_step_ = step;
    ctrace_phase_ = phase;
    ctrace_turn_ = turn;
    ctrace_seq_ = 0;
    ctrace_ask_layer_ = -1;
    ctrace_obs_layer_ = -1;
    // The first node of the graph is charged from here, so the mark must be taken as close to
    // llama_decode as the caller can manage — anything between them lands on node 0.
    ctrace_mark_ = std::chrono::steady_clock::now();
    ctrace_faults_ = pio::major_faults();
}

// Layer granularity: emit the closing row for the segment `interval_layer`, charged the wall
// and faults since the previous boundary. Shared by the observe path and end_compute_batch.
void RouterHook::ctrace_close_segment(int interval_layer, const char * tail_name) {
    const auto now = std::chrono::steady_clock::now();
    const uint64_t faults = pio::major_faults();
    ComputeTraceRow r;
    r.turn = ctrace_turn_;
    r.phase = ctrace_phase_;
    r.step = ctrace_step_;
    r.seq = ctrace_seq_++;
    r.layer = interval_layer;
    r.op = "LAYER";
    r.name = tail_name ? tail_name : (interval_layer < 0 ? "pre" : "blk." + std::to_string(interval_layer));
    r.wall_ns = (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(now - ctrace_mark_).count();
    r.majflt = faults - ctrace_faults_;
    compute_rows_.push_back(std::move(r));
    ctrace_mark_ = now;
    ctrace_faults_ = faults;
}

void RouterHook::end_compute_batch() {
    // Node granularity has no dangling interval: the last node was itself isolated and observed.
    // The tail row absorbs whatever ran between the last boundary and this call — keep the call
    // adjacent to llama_decode's return or the decode epilogue is billed to the LM head.
    if (!ctrace_on_ || !ctrace_layers_) return;
    ctrace_close_segment(-1, "post");
}

bool RouterHook::on_eval(ggml_tensor * t, bool ask) {
    // ── compute trace: close the previous node's interval, open the next ──
    // Ordering matters: this runs before every other job below, so the timestamp is as close to the
    // boundary as possible and the streamer's own work (load_layer, the residency query) lands
    // inside the routing node's interval where it belongs — that IS what routing costs here.
    if (ctrace_on_ && !ask) {
        if (ctrace_layers_) {
            // Only isolated nodes reach this branch: layer boundaries, plus the routing nodes the
            // streamer isolates anyway (a barrier that exists untraced too, so it is free to use).
            // The interval since the previous boundary belongs to the segment we are LEAVING —
            // attributing it to this node's layer would misfile nearly a whole layer, since a
            // boundary node is the first node of the next one.
            ctrace_close_segment(ctrace_obs_layer_, nullptr);
            const int nl = node_layer(t->name);
            if (nl >= 0) ctrace_obs_layer_ = nl; // layerless nodes (reshapes, views) don't move the cursor
        } else {
            const auto now = std::chrono::steady_clock::now();
            const uint64_t faults = pio::major_faults();
            ComputeTraceRow r;
            r.turn = ctrace_turn_;
            r.phase = ctrace_phase_;
            r.step = ctrace_step_;
            r.seq = ctrace_seq_++;
            r.layer = node_layer(t->name);
            r.op = ggml_op_name(t->op);
            r.name = t->name;
            r.wall_ns = (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(now - ctrace_mark_).count();
            r.majflt = faults - ctrace_faults_;
            compute_rows_.push_back(std::move(r));
            ctrace_mark_ = now;
            ctrace_faults_ = faults;
        }
    }

    // ── capture: harvest expert weight tensors from every node's sources ──
    if (capturing_) {
        if (ask) {
            for (int s = 0; s < GGML_MAX_SRC; ++s) {
                ggml_tensor * src = t->src[s];
                if (!src || src->name[0] == '\0') continue;
                int il = -1;
                const int p = match_expert(src->name, recipe_, il);
                if (p >= 0) {
                    // An expert-named tensor: streamed, never a dense-rebind candidate.
                    if (il >= 0 && il < n_layer_ && src->ne[2] > 0) { // expert dim is dim-2
                        LayerExperts & L = captured_[il];
                        L.bound = true;
                        L.proj[p].tensor = src;
                        L.proj[p].nb2 = (uint64_t) src->nb[2];
                    }
                    continue;
                }
                // A weight LEAF (op NONE, named) that is not an expert: the persistent dense weights
                // --dense-weights anon may rebind. Graph inputs and KV tensors share this shape but are
                // filtered out downstream by the gguf tensor set, so recording them here is harmless.
                if (src->op == GGML_OP_NONE) captured_weights_[src->name] = src;
            }
        }
        return false; // capture never isolates a node
    }

    // ── stream: the routing nodes get the single-node barrier so we see the selected ids ──
    int il = -1;
    const bool is_topk = std::sscanf(t->name, "ffn_moe_topk-%d", &il) == 1 && il >= 0;
    // The weight nodes are asked for by a traced run, and by the drop policy, which decides on the
    // weights the matmul will actually apply. Each extra ask is another barrier — a handful per MoE
    // layer, on tensors of a few floats — so neither is on by default.
    int wl = -1;
    const bool want_weights = trace_on_ || drop_armed();
    const bool is_weights = want_weights && match_weights(t->name, wl);
    // The gate matmul, asked for only by the prediction probe. Decode-only: prefill has no previous
    // token to predict from, and it is not the phase a prefetch would be built for. The "-" in the
    // pattern is what keeps "ffn_moe_logits_biased-<il>" — a different node — from matching.
    int gl = -1;
    const bool is_logits =
        predict_log_ && source_ && batch_phase_ == 1 && std::sscanf(t->name, "ffn_moe_logits-%d", &gl) == 1 && gl >= 0;
    // The compute trace wants every node isolated — or, at layer granularity, only the first
    // node of each layer: the cursor advances on the ask stream (every node passes through
    // here), so one isolation request per layer transition. Layerless names (embeddings, the
    // head, mid-layer reshapes) never move the cursor, so they cannot fake a boundary. The
    // streamer only wants the routing ones.
    if (ask) {
        bool ctrace_iso = false;
        if (ctrace_on_) {
            if (!ctrace_layers_) {
                ctrace_iso = true;
            } else {
                const int nl = node_layer(t->name);
                if (nl >= 0 && nl != ctrace_ask_layer_) {
                    ctrace_iso = true;
                    ctrace_ask_layer_ = nl;
                }
            }
        }
        return ctrace_iso || is_topk || is_weights || is_logits;
    }

    // The probe attaches to the gate matmul rather than to the topk node because this is where the
    // router's own two inputs are reachable: a graph intermediate cannot be found by name later, and
    // its pointer does not survive to the next graph. It writes nothing the graph will read.
    if (is_logits) predict_at_logits(t, gl);

    // Weights follow their layer's topk, so the pending record is already open; keep the last
    // one offered (match_weights explains why) and let the flush read it. This runs BEFORE the drop
    // policy edits the same tensor, so the trace records the routing the router produced, not the
    // one the policy left behind — `dropped` is what says which is which.
    if (is_weights && t->data && t->type == GGML_TYPE_F32 && pending_.layer == wl && pending_.nu > 0)
        gather_weights(t, pending_.nu, pending_.nt, pending_.weights);

    // Learn which node ends this layer's weight chain, and — once known — use it as the point where
    // the routing is decided: everything the drop policy needs is final here, and nothing has
    // consumed it yet. The learning pass and the deferral are the same walk, so a layer whose chain
    // shape the hook has not seen yet simply keeps the undropped behaviour.
    if (is_weights && t->data && t->type == GGML_TYPE_F32 && drop_.layer == wl) {
        chain_last_ = t->name;
        if (drop_.deferred && wl >= 0 && wl < (int) term_node_.size() && term_node_[wl] == t->name) apply_drop(t);
    }

    if (source_ && is_topk && t->data && t->type == GGML_TYPE_I32) {
        // selected_experts is [n_expert_used, n_tokens] but a VIEW of the full argsort
        // [n_expert, n_tokens]: its row stride is nb[1] (= n_expert*4), not
        // n_expert_used*4. Gather respecting the strides — a flat read would grab token
        // 0's sorted tail as token 1's experts, corrupting the KV cache.
        // The previous layer's weight chain has been fully offered by now.
        close_drop_layer();

        gathered_.clear();
        const int nu = (int) t->ne[0], nt = (int) t->ne[1];
        for (int j = 0; j < nt; ++j)
            for (int k = 0; k < nu; ++k)
                gathered_.push_back(
                    *(const int32_t *) ((const char *) t->data + (size_t) j * t->nb[1] + (size_t) k * t->nb[0]));

        if (trace_on_) {
            flush_pending(); // the previous layer's whole weight chain has been offered by now
            pending_.layer = il;
            pending_.nu = nu;
            pending_.nt = nt;
            pending_.ids = gathered_;
            pending_.weights.clear();
            pending_.dropped.clear();
            // Classify against the cache BEFORE load_layer makes these experts resident —
            // afterwards everything reads as a hit. Settle landed prefetches first, or an expert
            // a prefetch correctly guessed would be recorded as a miss.
            source_->settle_spec();
            pending_.residency.assign(gathered_.size(), (uint8_t) 0);
            source_->query_residency(il, gathered_.data(), (int) gathered_.size(), pending_.residency.data());
        }

        // Count what the ROUTER selected, here rather than inside apply_drop: a layer that is not
        // deferred yet (the first graph, or a phase the policy is not armed for) still routed these
        // experts, and a denominator that skipped them would report the drop rate as a fraction of
        // the wrong thing.
        if (drop_frac_ > 0.0f) experts_routed_ += (long long) gathered_.size();

        // Open the layer for the drop policy. Deferring the load is only safe once this layer's
        // terminal weight node is known — otherwise there is no callback left to decide in, and the
        // expert matmul would run against slots nothing loaded. First graph of a run: load here.
        const bool defer = drop_armed() && il >= 0 && il < (int) term_node_.size() && !term_node_[il].empty();
        drop_.layer = il;
        drop_.nu = nu;
        drop_.nt = nt;
        drop_.ids = t;
        drop_.deferred = defer;
        chain_last_.clear();
        if (defer)
            drop_ids_ = gathered_;
        else
            source_->load_layer(il, gathered_.data(), (int) gathered_.size());

        // Score the predictors against the routing the router just produced — before the record
        // below moves on, or the previous-token predictor would be graded on the answer itself.
        // The ids gathered above are still the router's own choice: the drop policy edits them at
        // a later node, so a probed run measures routing, not what the cache left of it.
        if (predict_log_ && batch_phase_ == 1 && il >= 0 && il < n_layer_ && nt > 0)
            score_layer(il, gathered_.data() + (size_t) (nt - 1) * nu, nu);

        // Temporal prefetch: hint the next K layers with what the PREVIOUS token routed there,
        // to be read on idle lanes while this layer computes.
        if (prefetch_layers_ > 0 && il >= 0 && il < n_layer_) {
            for (int k = 1; k <= prefetch_layers_; ++k) {
                const int tl = il + k;
                if (tl < n_layer_ && !prev_ids_[tl].empty())
                    source_->prefetch(tl, prev_ids_[tl].data(), (int) prev_ids_[tl].size());
            }
        }

        // Record this layer's routing (the last token's row during prefill) for the next token to
        // predict from. Both consumers read it: the prefetch above, and the probe's previous-token
        // predictor — which is that same bet, scored instead of acted on.
        if ((prefetch_layers_ > 0 || predict_log_) && il >= 0 && il < n_layer_) {
            std::vector<int32_t> & rec = prev_ids_[il];
            rec.clear();
            const int last = nt - 1;
            for (int k = 0; k < nu; ++k)
                rec.push_back(
                    *(const int32_t *) ((const char *) t->data + (size_t) last * t->nb[1] + (size_t) k * t->nb[0]));
        }
    }
    return true;
}

} // namespace bmoe
