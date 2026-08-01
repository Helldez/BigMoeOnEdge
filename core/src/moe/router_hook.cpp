#include "router_hook.h"

#include "ggml.h"
#include "../io/platform_io.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

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

// Every routing node this hook looks for — the topk, the weight chain, the gate matmul — is named
// "ffn_moe_<something>". A graph has thousands of nodes and the ask pass sees all of them, so this
// one comparison is what keeps the rest of the matching off the hot path entirely. `name` is a
// fixed-size array inside the tensor, so reading eight bytes of it is always in bounds.
static inline bool is_moe_node(const char * name) {
    return std::memcmp(name, "ffn_moe_", 8) == 0;
}

// Parse the "<il>" that llama.cpp appends after the '-' on a per-layer node name. Digits only, to
// the end of the string — the same strictness node_layer applies, and stricter than the "%d" this
// replaces, which would have read "12abc" as layer 12. Returns -1 on anything else.
static inline int parse_layer_tail(const char * p) {
    if (*p < '0' || *p > '9') return -1;
    int il = 0;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        il = il * 10 + (*p - '0');
        if (il > (1 << 20)) return -1; // a runaway digit run is not a layer index
    }
    return il;
}

// "<prefix><il>" → il, or -1 if the name does not have that shape. Used for the two singular
// routing nodes (the topk and the gate matmul), whose prefixes carry their own length.
static inline int match_layer_node(const char * name, std::string_view prefix) {
    return std::memcmp(name, prefix.data(), prefix.size()) == 0 ? parse_layer_tail(name + prefix.size()) : -1;
}

// The router-weight nodes build_moe_ffn emits per layer. The chain is ffn_moe_weights →
// (_softmax | _norm) → (_scaled), each an optional refinement of the previous, so the LAST one
// offered for a layer carries the weight actually applied to that layer's expert outputs.
// Last-wins is what keeps this architecture-independent: no table of which gating each model
// uses. The '-' check is load-bearing — it rejects "ffn_moe_weights_sum-3", which prefix-matches
// "ffn_moe_weights" but is a row sum, not a weight.
//
// It also makes the four names mutually exclusive, which is why the match can be reported as an
// INDEX rather than a name: that index is how a layer's terminal node is remembered, so learning it
// costs no allocation and comparing against it is an integer test — on a path that runs for every
// weight node of every layer of every token whenever the drop policy is armed.
static constexpr std::string_view kWeightNodes[] = {
    "ffn_moe_weights",
    "ffn_moe_weights_softmax",
    "ffn_moe_weights_norm",
    "ffn_moe_weights_scaled",
};

static int match_weights(const char * name, int & il_out) {
    for (int v = 0; v < (int) (sizeof(kWeightNodes) / sizeof(kWeightNodes[0])); ++v) {
        const std::string_view n = kWeightNodes[v];
        if (std::memcmp(name, n.data(), n.size()) != 0 || name[n.size()] != '-') continue;
        const int il = parse_layer_tail(name + n.size() + 1);
        if (il < 0) continue;
        il_out = il;
        return v;
    }
    return -1;
}

// Where token j's slot k lives inside a weight node. Single source of truth for the layout: the
// gather below and the drop policy's writer must agree, or the policy would zero another token's
// slot. Returns a mutable pointer; the gather takes a const tensor and only reads through it.
//
// The node is [1, nu, nt] as built (token j at nb[2], slot k at nb[1]) — except the norm variant,
// whose callback fires on the pre-reshape 2-D [nu, nt] (token j at nb[1], slot k at nb[0]). As with
// the topk ids these are views, so only the strides say where a token's row really starts.
//
// The routing's own width and batch size are what tell the two shapes apart. `ne[0] == 1` alone
// does NOT: a top-1 routing makes the 2-D node [1, nt], which passes that test and would be read
// with the token stride of a 3-D node — sending every token after the first to the wrong row, and
// the drop policy's writes to the wrong slot. Match the full extents instead. Both fit only when
// nu and nt are 1, where the two layouts name the same single element.
static float * weight_at(const ggml_tensor * t, int nu, int nt, int j, int k) {
    const bool fits_3d = t->ne[0] == 1 && t->ne[1] == nu && t->ne[2] == nt;
    const bool fits_2d = t->ne[0] == nu && t->ne[1] == nt && t->ne[2] == 1;
    // Neither fits only if the graph produced a shape build_moe_ffn does not emit; keep the old
    // test as the fallback rather than inventing an offset for a node we do not recognise.
    const bool three_d = fits_3d || (!fits_2d && t->ne[0] == 1);
    const size_t tok_nb = three_d ? t->nb[2] : t->nb[1];
    const size_t slot_nb = three_d ? t->nb[1] : t->nb[0];
    return (float *) ((char *) t->data + (size_t) j * tok_nb + (size_t) k * slot_nb);
}

static void gather_weights(const ggml_tensor * t, int nu, int nt, std::vector<float> & out) {
    out.assign((size_t) nu * nt, 0.0f);
    for (int j = 0; j < nt; ++j)
        for (int k = 0; k < nu; ++k)
            out[(size_t) j * nu + k] = *weight_at(t, nu, nt, j, k);
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
    term_variant_.assign(n_layer_ > 0 ? n_layer_ : 0, (int8_t) -1);
    drop_ = PendingDrop{};
    chain_last_ = -1;
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
            *weight_at(wt, nu, nt, j, k) = 0.0f;
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
                if (!drop_mask_[row + k]) *weight_at(wt, nu, nt, j, k) *= g;
        }
    }

    if (tracing) pending_.dropped = drop_mask_;
    source_->load_layer(D.layer, drop_ids_.data(), (int) drop_ids_.size());
    D.deferred = false;
    predict_after_load(D.layer);
    route_ahead_collect(D.layer);
}

// Finish with the layer whose topk we last saw: record which node ended its weight chain, so the
// next graph can decide there, and make sure nothing was left waiting on a node that never came.
void RouterHook::close_drop_layer() {
    PendingDrop & D = drop_;
    if (D.layer < 0) return;
    if (D.layer < (int) term_variant_.size() && term_variant_[D.layer] < 0 && chain_last_ >= 0)
        term_variant_[D.layer] = chain_last_;
    if (D.deferred && source_) {
        // The node we learned as terminal did not appear this time, so the deferral was never
        // honoured and this layer's matmul has already run against slots nothing loaded. Load the
        // routing now to keep the cache's accounting straight, and — more importantly — FORGET the
        // terminal node, so the next graph re-learns it and loads at the topk node meanwhile.
        // Deferring again on the same stale guess would repeat the fault every single token; one
        // bad layer in one token is recoverable, a standing bet against a graph that moved is not.
        source_->load_layer(D.layer, drop_ids_.data(), (int) drop_ids_.size());
        if (D.layer < (int) term_variant_.size()) term_variant_[D.layer] = -1;
        D.deferred = false;
        predict_after_load(D.layer);
        route_ahead_collect(D.layer);
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
    predict_reset();
}

void RouterHook::set_predict_prefetch(bool on, int spec_max) {
    pred_spec_max_ = spec_max >= 0 ? spec_max : 0;
    predict_prefetch_ = on;
    predict_reset();
    if (on && !pred_worker_.joinable()) pred_worker_ = std::thread([this] { predict_worker_main(); });
    if (!on) predict_worker_stop();
}

void RouterHook::set_route_ahead(int n) {
    route_ahead_ = n > 0 ? n : 0;
    const int nl = n_layer_ > 0 ? n_layer_ : 0;
    ra_pred_.assign((size_t) nl, std::vector<int32_t>{});
    ra_gate_q_.assign((size_t) nl, std::vector<int8_t>{});
    ra_gate_s_.assign((size_t) nl, std::vector<float>{});
    ra_overridden_ = ra_passthrough_ = ra_slots_ = ra_hits_ = 0;
    ra_tripped_ = false;
    ra_gemv_ns_.store(0);
    ra_gemv_jobs_.store(0);
    ra_issue_ns_ = 0;
    // The pointer stashes are normally sized by predict_reset(); route-ahead can be armed with
    // both probe and prefetch off, so make sure they exist rather than assume who ran first.
    if (gate_w_.size() != (size_t) nl) gate_w_.assign((size_t) nl, nullptr);
    if (h_t_.size() != (size_t) nl) h_t_.assign((size_t) nl, nullptr);
    // The GEMV runs on the shared prediction worker (predict_prefetch is mutually exclusive, so
    // the job slot is never contended); own the thread's lifecycle the same way it does.
    if (route_ahead_ > 0 && !pred_worker_.joinable()) pred_worker_ = std::thread([this] { predict_worker_main(); });
    if (route_ahead_ == 0 && !predict_prefetch_) predict_worker_stop();
}

RouterHook::~RouterHook() {
    predict_worker_stop();
}

void RouterHook::predict_worker_stop() {
    if (!pred_worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(pred_mtx_);
        pred_stop_ = true;
    }
    pred_cv_.notify_all();
    pred_worker_.join();
    pred_stop_ = false;
}

void RouterHook::predict_reset() {
    const int n = n_layer_ > 0 ? n_layer_ : 0;
    gate_w_.assign(n, nullptr);
    h_t_.assign(n, nullptr);
    pred_stale_.assign(n, std::vector<int32_t>{});
    pred_stale2_.assign(n, std::vector<int32_t>{});
    pred_self_.assign(n, std::vector<int32_t>{});
    ps_stale_.assign(n, PredictorStats{});
    ps_prev_.assign(n, PredictorStats{});
    ps_self_.assign(n, PredictorStats{});
    agg_stale_ = agg_stale2_ = agg_prev_ = agg_self_ = PredictorStats{};
    predict_unscored_ = 0;
    wd_slots_ = wd_hits_ = wd_rout_ = 0;
    wd_tripped_ = false;
    std::lock_guard<std::mutex> lk(pred_mtx_);
    pred_job_pending_ = false;
    pred_result_ = PredictResult{};
}

static bool gate_scores(const ggml_tensor * w, const std::vector<float> & h, std::vector<float> & out);

// The prediction worker: everything it touches is either job-local or a read-only weight leaf, so
// it needs no coordination with the graph, the source, or the LRU — the eval thread snapshots its
// inputs and consumes its outputs, and this thread only computes.
void RouterHook::predict_worker_main() {
    for (;;) {
        PredictJob job;
        {
            std::unique_lock<std::mutex> lk(pred_mtx_);
            pred_cv_.wait(lk, [&] { return pred_stop_ || pred_job_pending_; });
            if (pred_stop_) return;
            job = std::move(pred_job_);
            pred_job_pending_ = false;
        }
        std::vector<float> scores;
        // Time the GEMV itself (the ranking that follows is O(k*n_expert) and rides along): this
        // is the feature's own CPU bill, invisible in wall time wherever a spare core exists.
        const auto tg0 = std::chrono::steady_clock::now();
        // Commit jobs read the int8 mirror — a quarter of the bytes, which on a phone IS the cost:
        // the float GEMV there is bound by pulling ~4 MB of gate matrix per layer across a memory
        // bus the decode is already saturating. The probe's jobs always keep the exact weights,
        // because its whole purpose is to be a control. The mirror is built here, on this worker,
        // the first time a layer is predicted for.
        //
        // aarch64 only, and measured both ways: on x86 the float path is fully vectorized and DRAM
        // bandwidth is ample, so the int8 detour costs more than it saves (0.38 vs 0.26 ms per
        // GEMV on the host, same config). The bytes only matter where they are scarce.
        bool scored = false;
#if defined(__aarch64__)
        if (job.commit && job.nl >= 0 && quantize_gate(job.nl)) scored = gate_scores_q(job.nl, job.row, scores);
#endif
        if (!scored) scored = job.gate && gate_scores(job.gate, job.row, scores);
        if (job.commit) {
            ra_gemv_ns_.fetch_add(
                (long long) std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tg0)
                    .count());
            ra_gemv_jobs_.fetch_add(1);
        }
        if (!scored) continue;
        PredictResult r;
        r.seq = job.seq;
        r.nl = job.nl;
        r.commit = job.commit;
        if (job.commit) {
            // Rank exactly the routing's width, not the probe's max: rank_top_k is a partial
            // selection whose cost grows with the square of the width, so ranking 32 to use 8
            // was four times the work for entries nothing reads — measurable on a phone, where
            // this runs 40 times a token beside a decode competing for the same cores.
            rank_top_k(scores, job.nu, r.ids);
            // The issue list rides in r.spec: of the top-nu the commit will route, only the
            // experts the drop policy would actually READ. Same arithmetic as build_spec_lists
            // (softmax over the top-nu as a proxy for the routing weights, threshold at
            // drop_frac/nu, the top expert always kept) — an expert predicted below the drop
            // threshold is left cold ON PURPOSE, so the commit-time drop discards it unread
            // exactly as it does the router's own cold tail. Measured on device before this
            // filter existed: uncapped early reads made every committed expert resident, the
            // residency-gated drop stopped biting at all, and a run paid 3x the flash and half
            // the tok/s for a perfect prefetch of bytes the baseline never read (78 -> 244
            // MB/token at depth 4, drop 0.75). With drop off the threshold is 0 and the list
            // is simply the committed top-nu.
            const int nu = job.nu < (int) r.ids.size() ? job.nu : (int) r.ids.size();
            if (nu > 0) {
                float w[predict_max_k];
                float mx = scores[(size_t) r.ids[0]];
                for (int k = 1; k < nu; ++k)
                    if (scores[(size_t) r.ids[k]] > mx) mx = scores[(size_t) r.ids[k]];
                float sum = 0.0f;
                for (int k = 0; k < nu; ++k) {
                    w[k] = std::exp(scores[(size_t) r.ids[k]] - mx);
                    sum += w[k];
                }
                const float thr = job.drop_frac > 0.0f ? job.drop_frac / (float) nu : 0.0f;
                for (int k = 0; k < nu; ++k)
                    if (k == 0 || job.drop_frac <= 0.0f || (sum > 0.0f && w[k] / sum >= thr))
                        r.spec.push_back(r.ids[(size_t) k]);
            }
        } else {
            build_spec_lists(scores, job.nu, job.drop_frac, job.spec_max, job.resident, r.spec, r.keep);
        }
        r.ready = true;
        std::lock_guard<std::mutex> lk(pred_mtx_);
        pred_result_ = std::move(r);
    }
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
//
// The F16 path matters more than it looks. ggml_fp16_to_fp32 is an exported function, not an
// inlinable conversion, so the obvious loop pays a function call per weight element — 21M calls
// per token on a 40-layer, 256-expert model, which measured as ~35-45 ms per GEMV pass and
// dwarfed everything else this feature does. On aarch64 the compiler converts __fp16 natively
// (one instruction, vectorizable), so that path is used wherever it exists.
// The four-accumulator shape is not a micro-optimisation, it is what makes this loop vectorize at
// all: floating-point addition is not associative, so a compiler may not re-order a single-
// accumulator reduction, and the obvious `acc += p[d] * h[d]` compiles to one scalar multiply-add
// per cycle however high the optimisation level. Splitting the reduction into independent partial
// sums gives the vectorizer permission it cannot take on its own. Measured on the host: 0.77 ->
// 0.20 ms per gate pass on a 256-expert model — and this is the whole feature's per-layer tax, so
// on a 4-thread phone (where it competes with the decode for cores AND for DRAM bandwidth) it was
// the difference between a win and a rout. The partial sums change the summation order, so the
// last bits of a score may differ from the graph's own gate; the ranking is unaffected in every
// case that is not already a numerical tie, and the watchdog is what would catch it if it were.
static bool gate_scores(const ggml_tensor * w, const std::vector<float> & h, std::vector<float> & out) {
    const int64_t nd = w->ne[0], ne = w->ne[1];
    if (nd != (int64_t) h.size() || ne <= 0) return false;
    if (w->type != GGML_TYPE_F32 && w->type != GGML_TYPE_F16) return false;
    out.assign((size_t) ne, 0.0f);
    const float * hp = h.data();
    const int64_t nd4 = nd & ~(int64_t) 3;
    for (int64_t e = 0; e < ne; ++e) {
        const char * row = (const char *) w->data + (size_t) e * w->nb[1];
        float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        if (w->type == GGML_TYPE_F32) {
            const float * p = (const float *) row;
            for (int64_t d = 0; d < nd4; d += 4) {
                a0 += p[d + 0] * hp[d + 0];
                a1 += p[d + 1] * hp[d + 1];
                a2 += p[d + 2] * hp[d + 2];
                a3 += p[d + 3] * hp[d + 3];
            }
            for (int64_t d = nd4; d < nd; ++d)
                a0 += p[d] * hp[d];
        } else {
#if defined(__aarch64__)
            const __fp16 * p = (const __fp16 *) row;
#else
            const ggml_fp16_t * q = (const ggml_fp16_t *) row;
#endif
            for (int64_t d = 0; d < nd4; d += 4) {
#if defined(__aarch64__)
                a0 += (float) p[d + 0] * hp[d + 0];
                a1 += (float) p[d + 1] * hp[d + 1];
                a2 += (float) p[d + 2] * hp[d + 2];
                a3 += (float) p[d + 3] * hp[d + 3];
#else
                a0 += ggml_fp16_to_fp32(q[d + 0]) * hp[d + 0];
                a1 += ggml_fp16_to_fp32(q[d + 1]) * hp[d + 1];
                a2 += ggml_fp16_to_fp32(q[d + 2]) * hp[d + 2];
                a3 += ggml_fp16_to_fp32(q[d + 3]) * hp[d + 3];
#endif
            }
            for (int64_t d = nd4; d < nd; ++d)
#if defined(__aarch64__)
                a0 += (float) p[d] * hp[d];
#else
                a0 += ggml_fp16_to_fp32(q[d]) * hp[d];
#endif
        }
        out[(size_t) e] = (a0 + a1) + (a2 + a3);
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

// Build the int8 mirror of layer il's gate matrix (see the header for why). One symmetric scale
// per expert ROW keeps each row's dynamic range: the rows are what get compared against one
// another, and a single matrix-wide scale would flatten a quiet row into noise. Runs once per
// layer, on the prediction worker, reading a weight leaf nothing mutates.
bool RouterHook::quantize_gate(int il) {
    if (il < 0 || il >= (int) ra_gate_q_.size()) return false;
    if (!ra_gate_q_[il].empty()) return true; // already mirrored
    const ggml_tensor * w = gate_w_[il];
    if (!w || !w->data) return false;
    if (w->type != GGML_TYPE_F32 && w->type != GGML_TYPE_F16) return false;
    const int64_t nd = w->ne[0], ne = w->ne[1];
    if (nd <= 0 || ne <= 0) return false;

    std::vector<int8_t> q((size_t) nd * (size_t) ne);
    std::vector<float> s((size_t) ne, 0.0f);
    std::vector<float> row((size_t) nd);
    for (int64_t e = 0; e < ne; ++e) {
        const char * src = (const char *) w->data + (size_t) e * w->nb[1];
        float amax = 0.0f;
        for (int64_t d = 0; d < nd; ++d) {
            float v;
            if (w->type == GGML_TYPE_F32) {
                v = ((const float *) src)[d];
            } else {
#if defined(__aarch64__)
                v = (float) ((const __fp16 *) src)[d];
#else
                v = ggml_fp16_to_fp32(((const ggml_fp16_t *) src)[d]);
#endif
            }
            row[(size_t) d] = v;
            const float a = v < 0.0f ? -v : v;
            if (a > amax) amax = a;
        }
        const float scale = amax > 0.0f ? amax / 127.0f : 0.0f;
        s[(size_t) e] = scale;
        const float inv = scale > 0.0f ? 1.0f / scale : 0.0f;
        int8_t * dst = q.data() + (size_t) e * (size_t) nd;
        for (int64_t d = 0; d < nd; ++d) {
            const float t = row[(size_t) d] * inv;
            const int v = (int) (t < 0.0f ? t - 0.5f : t + 0.5f);
            dst[d] = (int8_t) (v < -127 ? -127 : (v > 127 ? 127 : v));
        }
    }
    ra_gate_q_[il] = std::move(q);
    ra_gate_s_[il] = std::move(s);
    return true;
}

// The prediction's GEMV against the int8 mirror. The ACTIVATION is quantized too, once per call,
// so the inner loop is int8 x int8 accumulated in int32 — the shape ARM's dotprod instructions
// exist for (this build targets armv8.2-a+dotprod), and the shape a compiler will vectorize
// without needing permission to reorder a float reduction. Both effects point the same way on a
// phone: a quarter of the bytes off the memory bus, and integer MACs instead of float ones.
// Ranking only needs the ORDER of the scores, and both scales are positive, so the per-expert
// scale is applied at the end where it costs one multiply per expert instead of one per element.
bool RouterHook::gate_scores_q(int il, const std::vector<float> & h, std::vector<float> & out) {
    if (il < 0 || il >= (int) ra_gate_q_.size() || ra_gate_q_[il].empty()) return false;
    const ggml_tensor * w = gate_w_[il];
    if (!w) return false;
    const int64_t nd = w->ne[0], ne = w->ne[1];
    if (nd != (int64_t) h.size() || (size_t) nd * (size_t) ne != ra_gate_q_[il].size()) return false;

    // Quantize the activation row: one pass over n_embd, amortised across all n_expert rows.
    float amax = 0.0f;
    for (int64_t d = 0; d < nd; ++d) {
        const float a = h[(size_t) d] < 0.0f ? -h[(size_t) d] : h[(size_t) d];
        if (a > amax) amax = a;
    }
    if (!(amax > 0.0f)) return false; // a zero row says nothing; let the caller fall back
    const float hs = amax / 127.0f;
    std::vector<int8_t> & qh = ra_qh_;
    qh.resize((size_t) nd);
    {
        const float inv = 1.0f / hs;
        for (int64_t d = 0; d < nd; ++d) {
            const float t = h[(size_t) d] * inv;
            const int v = (int) (t < 0.0f ? t - 0.5f : t + 0.5f);
            qh[(size_t) d] = (int8_t) (v < -127 ? -127 : (v > 127 ? 127 : v));
        }
    }

    const int8_t * q = ra_gate_q_[il].data();
    const float * sc = ra_gate_s_[il].data();
    const int8_t * hp = qh.data();
    const int64_t nd4 = nd & ~(int64_t) 3;
    out.assign((size_t) ne, 0.0f);
    for (int64_t e = 0; e < ne; ++e) {
        const int8_t * p = q + (size_t) e * (size_t) nd;
        int32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        for (int64_t d = 0; d < nd4; d += 4) {
            a0 += (int32_t) p[d + 0] * (int32_t) hp[d + 0];
            a1 += (int32_t) p[d + 1] * (int32_t) hp[d + 1];
            a2 += (int32_t) p[d + 2] * (int32_t) hp[d + 2];
            a3 += (int32_t) p[d + 3] * (int32_t) hp[d + 3];
        }
        for (int64_t d = nd4; d < nd; ++d)
            a0 += (int32_t) p[d] * (int32_t) hp[d];
        out[(size_t) e] = (float) ((a0 + a1) + (a2 + a3)) * sc[e] * hs;
    }
    return true;
}

// At the topk of layer il, BEFORE the commit overwrites the ids: sample the watchdog against the
// router's own choice, copy the gate-input row, and submit the ranking job for layer il+N to the
// worker. This is the prefetch's barrier-less read (the row was stashed in the ask pass and is
// only PROBABLY still intact by now — ggml's planner may reuse the buffer), made safe for a
// committed consumer by two things: the sampled fresh-gate control below, which disarms the whole
// policy out loud if the read stops reproducing the router; and the passthrough default, which
// keeps any unproven layer on the router's own routing.
void RouterHook::route_ahead_submit(ggml_tensor * ids, int il, int nu, int nt) {
    if (ra_tripped_ || il < 0 || il >= n_layer_ || nu <= 0 || nu > predict_max_k || nt <= 0) return;
    // Invalidate this token's slot for the target layer FIRST, before any early return below can
    // skip it: what sits there is the PREVIOUS token's ranking for that layer, and a submit that
    // declines (an unstashed gate, an unreadable row) would otherwise leave it in place for
    // apply_route_ahead to commit — routing a token by a prediction made from another token's
    // hidden state. Passthrough is the correct behaviour for a layer this token cannot predict.
    const int target = il + route_ahead_;
    if (target < n_layer_) ra_pred_[target].clear();
    ggml_tensor * h = h_t_[il];
    if (!h || !h->data || h->ne[1] <= 0) return;
    const int j = (int) h->ne[1] - 1; // the batch's last token, the row every predictor here uses

    // Watchdog: the layer's OWN gate on the just-copied row must reproduce the ids the router just
    // chose — read from the tensor now, before apply_route_ahead rewrites them. Same cadence and
    // thresholds as the prefetch's; the difference is what a trip protects against.
    if (++wd_rout_ % wd_interval == 0) {
        // The sampled control is an exact float GEMV on the EVAL thread — cheap because it is
        // sampled, but it lives in the compute residual, so it carries its own meter.
        const auto tw0 = std::chrono::steady_clock::now();
        if (gate_w_[il] && row_to_float(h, j, pred_row_) && gate_scores(gate_w_[il], pred_row_, pred_scores_)) {
            std::vector<int32_t> ctrl;
            rank_top_k(pred_scores_, nu, ctrl);
            int hits = 0;
            for (int k = 0; k < nu; ++k) {
                const int32_t actual = *id_at(ids, nt - 1, k);
                for (int32_t p : ctrl)
                    if (p == actual) {
                        ++hits;
                        break;
                    }
            }
            wd_slots_ += nu;
            wd_hits_ += hits;
            if (wd_slots_ >= wd_min_slots && (double) wd_hits_ < wd_min_frac * (double) wd_slots_) {
                ra_tripped_ = true;
                std::fprintf(stderr,
                             "bmoe: route-ahead disarmed — control at %.1f%% over %lld slots says the "
                             "barrier-less gate-input read is not trustworthy on this graph; routing "
                             "stays the router's own from here\n",
                             100.0 * wd_hits_ / wd_slots_, wd_slots_);
            }
        }
        ra_wd_ns_ +=
            (long long) std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tw0)
                .count();
        if (ra_tripped_) return;
    }

    const int nl = target;
    if (nl >= n_layer_) return;
    const ggml_tensor * wn = gate_w_[nl];
    if (!wn || !wn->data) return; // target matrix not stashed yet (first decode token): passthrough
    if (!row_to_float(h, j, pred_row_)) return;
    {
        std::lock_guard<std::mutex> lk(pred_mtx_);
        pred_job_.seq = ++pred_seq_;
        pred_job_.nl = nl;
        pred_job_.nu = nu;
        pred_job_.commit = true;
        pred_job_.drop_frac = drop_frac_; // the issue list is drop-aware; the commit is not
        pred_job_.gate = wn;
        pred_job_.row = pred_row_;
        pred_job_pending_ = true;
    }
    pred_cv_.notify_one();
}

// Pop a finished commit ranking, right after layer il's load was handed to the source — the same
// post-load window every speculation issues in. A result for a layer still ahead of il gets its
// reads started HERE: that is the route-ahead window paying out, N-1 layers of compute before the
// committing topk even runs. Only the latest submission is accepted; a ranking the previous token
// never collected fails the seq check and dies here rather than routing anything.
void RouterHook::route_ahead_collect(int il) {
    if (route_ahead_ <= 0 || ra_tripped_ || batch_phase_ != 1 || !source_) return;
    PredictResult r;
    {
        // Never waited for, only popped — measured, not a stylistic choice: a bounded cv wait here
        // (to force every committed layer's reads to start at the predicting layer) bought I/O
        // 0.060→0.040 s/token on the host and paid +0.045 s/token of eval-thread wall for it,
        // because a cv wakeup costs on the order of a millisecond, i.e. the demand read it was
        // trying to save. The depth knob is the honest lever instead: at N>=2 the ranking is
        // simply ready a whole layer before its topk, and the next callback issues it with no one
        // waiting on anyone.
        std::lock_guard<std::mutex> lk(pred_mtx_);
        if (!pred_result_.ready || !pred_result_.commit || pred_result_.seq != pred_seq_) return;
        r = std::move(pred_result_);
        pred_result_.ready = false;
    }
    if (r.nl <= 0 || r.nl >= n_layer_ || r.ids.empty()) return;
    ra_pred_[r.nl] = std::move(r.ids);
    // r.spec is the drop-aware issue list the worker built alongside the ranking: the committed
    // experts the drop policy would actually read. Issue only those; the rest stay cold and die
    // at the commit's drop exactly as the router's own cold tail does.
    if (r.nl > il) route_ahead_issue_for(r.nl, r.spec);
}

// At the topk of layer il, before anything reads the ids: replace the router's selection with the
// ranking made N layers back. This runs BEFORE the weight chain's get_rows consumes the ids, so
// the graph itself computes the substituted experts' weights from this layer's true logits — the
// committed routing carries real, renormalized probabilities, not copies of the prediction's.
// Rewriting here also means the gather below, the trace, the drop policy and load_layer all see
// the committed ids: the whole pipeline treats them as THE routing, which is the experiment.
void RouterHook::apply_route_ahead(ggml_tensor * ids, int il, int nu, int nt) {
    if (ra_tripped_ || il < 0 || il >= n_layer_ || nu <= 0 || nu > predict_max_k) return;
    if (il < route_ahead_) return; // structurally no prediction reaches these layers; not a miss
    std::vector<int32_t> & pred = ra_pred_[il];
    // One-token decode rows only: the prediction was made from a single hidden-state row. A wider
    // decode batch has no per-token prediction to substitute, so it keeps the router's choice.
    if (nt != 1 || (int) pred.size() < nu) {
        ++ra_passthrough_;
        pred.clear();
        return;
    }
    // The agreement between the committed selection and the router's own choice IS the measured
    // perturbation this flag trades on; score it before the overwrite destroys the evidence.
    int hits = 0;
    for (int k = 0; k < nu; ++k) {
        const int32_t actual = *id_at(ids, 0, k);
        for (int p = 0; p < nu; ++p)
            if (pred[(size_t) p] == actual) {
                ++hits;
                break;
            }
    }
    ra_slots_ += nu;
    ra_hits_ += hits;
    ++ra_overridden_;
    for (int k = 0; k < nu; ++k)
        *id_at(ids, 0, k) = pred[(size_t) k];
    pred.clear();
}

// Start reading layer nl's experts ahead of its topk. `cand` is the worker's drop-aware issue
// list: committed experts the drop policy would actually read, confidence-ordered. Within that
// list no spec_max cap applies — these are not guesses, and a miss not issued here is the same
// read paid on demand later, minus the window. Residents are retained instead (protecting a
// slice that is certainly about to be routed costs zero bytes). Runs on the eval thread, from a
// collect point that sits after some layer's own load was handed to the source — the same
// post-load issue window every other speculation uses, so the demand reads in flight keep their
// head-of-line position.
void RouterHook::route_ahead_issue_for(int nl, const std::vector<int32_t> & cand) {
    if (route_ahead_ <= 0 || batch_phase_ != 1 || !source_) return;
    if (nl <= 0 || nl >= n_layer_ || cand.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    ra_ids_.assign(cand.begin(), cand.end());
    // Settle landed speculation first, or a slice already read for an earlier token would classify
    // as a miss and be read again — the exact double-spend settle_spec exists to prevent.
    source_->settle_spec();
    ra_res_.assign(ra_ids_.size(), (uint8_t) 0);
    source_->query_residency(nl, ra_ids_.data(), (int) ra_ids_.size(), ra_res_.data());
    ra_keep_.clear();
    ra_spec_.clear();
    for (size_t i = 0; i < ra_ids_.size(); ++i)
        (ra_res_[i] != route_miss ? ra_keep_ : ra_spec_).push_back(ra_ids_[i]);
    if (!ra_keep_.empty()) source_->retain(nl, ra_keep_.data(), (int) ra_keep_.size());
    if (!ra_spec_.empty()) source_->prefetch(nl, ra_spec_.data(), (int) ra_spec_.size());
    ra_issue_ns_ +=
        (long long) std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
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
    // also the check that this GEMV agrees with llama.cpp's. Probe-only — the prefetch has no use
    // for a prediction of the layer it is already standing in, and skipping it halves the tax.
    if (predict_log_ && gate_w_[il] && gate_scores(gate_w_[il], pred_row_, pred_scores_))
        rank_top_k(pred_scores_, predict_max_k, pred_self_[il]);

    // Stale-gate: the NEXT layer's matrix on this layer's input — the prediction under test. Silent
    // on the first token of a run, whose next layer has not been seen yet; score_layer counts those.
    if (nl < n_layer_ && gate_w_[nl] && gate_scores(gate_w_[nl], pred_row_, pred_scores_))
        rank_top_k(pred_scores_, predict_max_k, pred_stale_[nl]);

    // Stale-2: the matrix two layers ahead on the same input — the accuracy the ASYNC prefetch
    // actually runs at (see predict_at_topk for why its horizon is two). Probe-only bookkeeping;
    // the aggregate is what prices the extra layer of staleness on a given model.
    const int n2 = il + 2;
    if (predict_log_ && n2 < n_layer_) {
        pred_stale2_[n2].clear();
        if (gate_w_[n2] && gate_scores(gate_w_[n2], pred_row_, pred_scores_))
            rank_top_k(pred_scores_, predict_max_k, pred_stale2_[n2]);
    }
}

// From one full score vector to the two lists the prefetch acts on. The softmax over the top-nu
// predicted scores approximates the routing weights the real gate will produce (exact for
// softmax-gated models, a fair proxy elsewhere); a candidate below the drop threshold is only ever
// read on demand — if it misses, the policy drops it unread, so speculating it would spend the
// exact I/O the policy exists to save. The top prediction is always kept, mirroring the policy's
// own pin of the top-weighted expert.
//
// The split by residency is the cost model in one line: an absent predicted expert is worth AT
// MOST spec_max flash reads (head-of-line stall is all a prefetch can remove — the tail is
// already hidden behind the expert matmul, and speculating a full top-8 was measured to read 33%
// more flash per token and to triple major faults against a full cache); a RESIDENT predicted
// expert costs nothing to keep and is merely retained, protecting it from an eviction that would
// turn a predicted hit back into a read.
void RouterHook::build_spec_lists(const std::vector<float> & scores,
                                  int nu,
                                  float drop_frac,
                                  int spec_max,
                                  const std::vector<uint8_t> & resident,
                                  std::vector<int32_t> & spec,
                                  std::vector<int32_t> & keep) {
    spec.clear();
    keep.clear();
    std::vector<int32_t> pred;
    rank_top_k(scores, nu, pred);
    if ((int) pred.size() < nu) return;

    float w[predict_max_k];
    float mx = scores[(size_t) pred[0]];
    for (int k = 1; k < nu; ++k)
        if (scores[(size_t) pred[k]] > mx) mx = scores[(size_t) pred[k]];
    float sum = 0.0f;
    for (int k = 0; k < nu; ++k) {
        w[k] = std::exp(scores[(size_t) pred[k]] - mx);
        sum += w[k];
    }
    const float thr = drop_frac > 0.0f ? drop_frac / (float) nu : 0.0f;
    for (int k = 0; k < nu; ++k) {
        if (k != 0 && drop_frac > 0.0f && !(sum > 0.0f && w[k] / sum >= thr)) continue;
        const int32_t e = pred[k];
        if (e >= 0 && (size_t) e < resident.size() && resident[(size_t) e] != route_miss)
            keep.push_back(e);
        else if ((int) spec.size() < spec_max)
            spec.push_back(e);
    }
}

// At the topk of layer il, with the routing ids in hand: sample the watchdog, then submit the
// prediction job for layer il+2 built from THIS layer's gate input.
void RouterHook::predict_at_topk(ggml_tensor * t, int il, int nu, int nt) {
    if (!predict_prefetch_ || wd_tripped_ || batch_phase_ != 1 || !source_) return;
    if (il < 0 || il >= n_layer_ || nu <= 0 || nu > predict_max_k || nt <= 0) return;
    ggml_tensor * h = h_t_[il];
    if (!h || !h->data || h->ne[1] <= 0) return;
    const int j = (int) h->ne[1] - 1; // the batch's last token

    // Watchdog sample: the layer's OWN gate on the row just read must reproduce the ids the router
    // just chose. This is the barrier-less read validating itself — if ggml's planner reused the
    // buffer between the gate matmul and here, or the architecture does not select by raw-logit
    // ranking, the control collapses and the prefetch disarms instead of speculating on noise.
    if (++wd_rout_ % wd_interval == 0 && gate_w_[il] && row_to_float(h, j, pred_row_) &&
        gate_scores(gate_w_[il], pred_row_, pred_scores_)) {
        std::vector<int32_t> ctrl;
        rank_top_k(pred_scores_, nu, ctrl);
        const int32_t * actual = gathered_.data() + (size_t) (nt - 1) * nu;
        int hits = 0;
        for (int k = 0; k < nu; ++k)
            for (int32_t p : ctrl)
                if (p == actual[k]) {
                    ++hits;
                    break;
                }
        wd_slots_ += nu;
        wd_hits_ += hits;
        if (wd_slots_ >= wd_min_slots && (double) wd_hits_ < wd_min_frac * (double) wd_slots_) {
            wd_tripped_ = true;
            std::fprintf(stderr,
                         "bmoe: predict-prefetch disarmed — control at %.1f%% over %lld slots says the "
                         "barrier-less gate-input read is not trustworthy on this graph\n",
                         100.0 * wd_hits_ / wd_slots_, wd_slots_);
            return;
        }
    }

    // Submit il+2: copy the row and snapshot residency on this thread (both are cheap and neither
    // is safe to read from the worker), and let the worker do the arithmetic.
    const int nl = il + 2;
    if (nl >= n_layer_) return;
    const ggml_tensor * wn = gate_w_[nl];
    if (!wn || !wn->data) return;
    const int ne = (int) wn->ne[1];
    if (ne <= 0 || !row_to_float(h, j, pred_row_)) return;

    spec_ids_.resize((size_t) ne); // scratch: the identity id list for the residency snapshot
    for (int e = 0; e < ne; ++e)
        spec_ids_[(size_t) e] = e;
    pred_res_.assign((size_t) ne, (uint8_t) 0);
    source_->query_residency(nl, spec_ids_.data(), ne, pred_res_.data());

    {
        std::lock_guard<std::mutex> lk(pred_mtx_);
        pred_job_.seq = ++pred_seq_;
        pred_job_.nl = nl;
        pred_job_.nu = nu;
        pred_job_.drop_frac = drop_frac_;
        pred_job_.spec_max = pred_spec_max_;
        pred_job_.gate = wn;
        pred_job_.row = pred_row_;
        pred_job_.resident = pred_res_;
        pred_job_pending_ = true;
    }
    pred_cv_.notify_one();
}

// Collect and act on a finished prediction, right after layer il's experts were handed to the
// source — the one moment a queued read has a whole layer's worth of idle-lane time before the
// next quiesce, whichever load path (plain topk, deferred drop, or the drop fallback) just ran.
// The result present here targets il+1 (it was submitted one layer back, so the worker had a full
// layer to make the deadline); one that is not ready is skipped, never waited for.
void RouterHook::predict_after_load(int il) {
    if (!predict_prefetch_ || wd_tripped_ || batch_phase_ != 1 || !source_) return;
    const int nl = il + 1;
    if (nl <= 0 || nl >= n_layer_) return;
    PredictResult r;
    {
        std::lock_guard<std::mutex> lk(pred_mtx_);
        if (!pred_result_.ready || pred_result_.nl != nl) return;
        r = std::move(pred_result_);
        pred_result_.ready = false;
    }
    // Retention first: it is free, and an entry protected before the eviction that a subsequent
    // prefetch commit might force is protected in time.
    if (!r.keep.empty()) source_->retain(nl, r.keep.data(), (int) r.keep.size());
    if (!r.spec.empty()) source_->prefetch(nl, r.spec.data(), (int) r.spec.size());
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
    if (!pred_stale2_[il].empty()) {
        // Aggregate only: the two-layer horizon exists to price the async prefetch's staleness,
        // and one number does that; a per-layer table of it would double the report for no call
        // anyone makes differently.
        PredictorStats discard;
        score_prediction(pred_stale2_[il], actual, nu, discard, agg_stale2_);
        pred_stale2_[il].clear();
    }
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
    // One prefix test decides whether any of the three matchers below can possibly fire. The vast
    // majority of a graph's nodes fail it and leave having done nothing else.
    const bool moe_node = is_moe_node(t->name);
    const int il = moe_node ? match_layer_node(t->name, "ffn_moe_topk-") : -1;
    const bool is_topk = il >= 0;
    // The weight nodes are asked for by a traced run, and by the drop policy, which decides on the
    // weights the matmul will actually apply. Each extra ask is another barrier — a handful per MoE
    // layer, on tensors of a few floats — so neither is on by default.
    int wl = -1;
    const bool want_weights = trace_on_ || drop_armed();
    const int weight_variant = (moe_node && want_weights) ? match_weights(t->name, wl) : -1;
    const bool is_weights = weight_variant >= 0;
    // Which of them is worth a BARRIER is a narrower question than which of them we recognise. The
    // chain's earlier nodes exist only so the terminal one can be identified; once a layer's
    // terminal is known, both consumers — the drop policy's decision point and the trace's
    // last-wins gather — read that node and no other. Isolating the rest costs a graph split and a
    // full compute-thread synchronization each, per layer, per token, on tensors of a few floats.
    //
    // Ask for the whole chain only while the layer is still learning its shape (term_variant_ < 0),
    // which is the first graph of a run and any graph after close_drop_layer forgets a terminal
    // that stopped appearing. That re-widening is what keeps this self-correcting: a graph that
    // moves is detected exactly as before, because the deferral still fails to be honoured.
    const bool weights_iso = is_weights && (wl < 0 || wl >= (int) term_variant_.size() || term_variant_[wl] < 0 ||
                                            term_variant_[wl] == weight_variant);
    // The gate matmul. The probe ISOLATES it (a barrier per layer — a probed run is not a
    // benchmark run); the prefetch only wants its source pointers, which the ask pass hands over
    // for free, so it asks for nothing and validates its barrier-less read with the watchdog
    // instead. Decode-only either way. The "-" in the pattern is what keeps
    // "ffn_moe_logits_biased-<il>" — a different node — from matching.
    const int gl = (moe_node && (predict_on() || route_ahead_ > 0) && source_ && batch_phase_ == 1)
                       ? match_layer_node(t->name, "ffn_moe_logits-")
                       : -1;
    const bool is_logits = gl >= 0;
    if (ask && is_logits && gl < n_layer_) {
        ggml_tensor * w = t->src[0];
        if (w && w->op == GGML_OP_NONE && w->data && ggml_is_contiguous(w)) gate_w_[gl] = w;
        h_t_[gl] = t->src[1];
    }
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
        // Route-ahead deliberately does NOT isolate the gate matmul: it wants the node's source
        // pointers (free, from this ask pass) and reads the row barrier-less at the topk, exactly
        // as the prefetch does — the isolated variant measured ~+0.04 s/token of pure barrier and
        // GEMV tax on the host. What makes that safe for a COMMITTED consumer is the watchdog in
        // route_ahead_submit plus the passthrough default, not a barrier.
        return ctrace_iso || is_topk || weights_iso || (is_logits && predict_log_);
    }

    // The probe attaches to the gate matmul rather than to the topk node because this is where the
    // router's own two inputs are reachable: a graph intermediate cannot be found by name later, and
    // its pointer does not survive to the next graph. It writes nothing the graph will read.
    if (is_logits && predict_on()) predict_at_logits(t, gl);

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
        chain_last_ = (int8_t) weight_variant;
        if (drop_.deferred && wl >= 0 && wl < (int) term_variant_.size() && term_variant_[wl] == weight_variant)
            apply_drop(t);
    }

    if (source_ && is_topk && t->data && t->type == GGML_TYPE_I32) {
        // selected_experts is [n_expert_used, n_tokens] but a VIEW of the full argsort
        // [n_expert, n_tokens]: its row stride is nb[1] (= n_expert*4), not
        // n_expert_used*4. Gather respecting the strides — a flat read would grab token
        // 0's sorted tail as token 1's experts, corrupting the KV cache.
        // The previous layer's weight chain has been fully offered by now.
        close_drop_layer();

        const int nu = (int) t->ne[0], nt = (int) t->ne[1];
        // Route-ahead, in submission order: first the watchdog sample and the ranking job for
        // layer il+N (both need the ROUTER's ids, still untouched here), then the commit — BEFORE
        // the ids are gathered, so everything downstream — the trace, the drop policy, load_layer,
        // the weight chain the graph is about to run — sees the committed routing, not the
        // router's, and nothing disagrees about which experts this layer used.
        if (route_ahead_ > 0 && batch_phase_ == 1) {
            // Collect FIRST: the ranking racing this topk is keyed to the seq of the job the
            // PREVIOUS topk submitted, and the submit below bumps that seq — collecting after it
            // would stale-fail nearly every legitimate result (measured: half the layers fell to
            // passthrough exactly that way, surviving only when a slow load let an earlier
            // collect site catch them).
            route_ahead_collect(il);
            route_ahead_submit(t, il, nu, nt);
            apply_route_ahead(t, il, nu, nt);
        }

        gathered_.clear();
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

        // Watchdog sample + submit the il+2 prediction job. Before the load flow on purpose: the
        // snapshot must precede load_layer, whose staging would otherwise count this layer's own
        // reads into the residency picture the job carries.
        predict_at_topk(t, il, nu, nt);

        // Open the layer for the drop policy. Deferring the load is only safe once this layer's
        // terminal weight node is known — otherwise there is no callback left to decide in, and the
        // expert matmul would run against slots nothing loaded. First graph of a run: load here.
        const bool defer = drop_armed() && il >= 0 && il < (int) term_variant_.size() && term_variant_[il] >= 0;
        drop_.layer = il;
        drop_.nu = nu;
        drop_.nt = nt;
        drop_.ids = t;
        drop_.deferred = defer;
        chain_last_ = -1;
        if (defer) {
            drop_ids_ = gathered_;
        } else {
            source_->load_layer(il, gathered_.data(), (int) gathered_.size());
            // Deferred layers speculate from apply_drop instead — after THEIR load, for the same
            // reason this call sits after the one above: the load's quiesce would cancel it.
            predict_after_load(il);
            route_ahead_collect(il);
        }

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
