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
static void gather_weights(const ggml_tensor * t, int nu, int nt, std::vector<float> & out) {
    const bool three_d = t->ne[0] == 1;
    const size_t tok_nb = three_d ? t->nb[2] : t->nb[1];
    const size_t slot_nb = three_d ? t->nb[1] : t->nb[0];
    out.assign((size_t) nu * nt, 0.0f);
    for (int j = 0; j < nt; ++j)
        for (int k = 0; k < nu; ++k)
            out[(size_t) j * nu + k] =
                *(const float *) ((const char *) t->data + (size_t) j * tok_nb + (size_t) k * slot_nb);
}

void RouterHook::begin_capture() {
    capturing_ = true;
    for (auto & L : captured_)
        L = LayerExperts{};
}
void RouterHook::end_capture() {
    capturing_ = false;
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
            if (r.residency == route_miss && charged_.insert(r.expert).second) r.expert_bytes = ebytes;
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

void RouterHook::set_compute_trace(bool on) {
    ctrace_on_ = on;
    compute_rows_.clear();
}

void RouterHook::begin_compute_batch(int step, int phase, int turn) {
    ctrace_step_ = step;
    ctrace_phase_ = phase;
    ctrace_turn_ = turn;
    ctrace_seq_ = 0;
    // The first node of the graph is charged from here, so the mark must be taken as close to
    // llama_decode as the caller can manage — anything between them lands on node 0.
    ctrace_mark_ = std::chrono::steady_clock::now();
    ctrace_faults_ = pio::major_faults();
}

bool RouterHook::on_eval(ggml_tensor * t, bool ask) {
    // ── compute trace: close the previous node's interval, open the next ──
    // Ordering matters: this runs before every other job below, so the timestamp is as close to the
    // boundary as possible and the streamer's own work (load_layer, the residency query) lands
    // inside the routing node's interval where it belongs — that IS what routing costs here.
    if (ctrace_on_ && !ask) {
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

    // ── capture: harvest expert weight tensors from every node's sources ──
    if (capturing_) {
        if (ask) {
            for (int s = 0; s < GGML_MAX_SRC; ++s) {
                ggml_tensor * src = t->src[s];
                if (!src || src->name[0] == '\0') continue;
                int il = -1;
                const int p = match_expert(src->name, recipe_, il);
                if (p < 0 || il < 0 || il >= n_layer_) continue;
                if (src->ne[2] <= 0) continue; // expert dim is dim-2
                LayerExperts & L = captured_[il];
                L.bound = true;
                L.proj[p].tensor = src;
                L.proj[p].nb2 = (uint64_t) src->nb[2];
            }
        }
        return false; // capture never isolates a node
    }

    // ── stream: the routing nodes get the single-node barrier so we see the selected ids ──
    int il = -1;
    const bool is_topk = std::sscanf(t->name, "ffn_moe_topk-%d", &il) == 1 && il >= 0;
    // Only a traced run asks for the weight nodes: each extra ask is another barrier.
    int wl = -1;
    const bool is_weights = trace_on_ && match_weights(t->name, wl);
    // The compute trace wants every node isolated; the streamer only wants the routing ones.
    if (ask) return ctrace_on_ || is_topk || is_weights;

    // Weights follow their layer's topk, so the pending record is already open; keep the last
    // one offered (match_weights explains why) and let the flush read it.
    if (is_weights && t->data && t->type == GGML_TYPE_F32 && pending_.layer == wl && pending_.nu > 0)
        gather_weights(t, pending_.nu, pending_.nt, pending_.weights);

    if (source_ && is_topk && t->data && t->type == GGML_TYPE_I32) {
        // selected_experts is [n_expert_used, n_tokens] but a VIEW of the full argsort
        // [n_expert, n_tokens]: its row stride is nb[1] (= n_expert*4), not
        // n_expert_used*4. Gather respecting the strides — a flat read would grab token
        // 0's sorted tail as token 1's experts, corrupting the KV cache.
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
            // Classify against the cache BEFORE load_layer makes these experts resident —
            // afterwards everything reads as a hit. Settle landed prefetches first, or an expert
            // a prefetch correctly guessed would be recorded as a miss.
            source_->settle_spec();
            pending_.residency.assign(gathered_.size(), (uint8_t) 0);
            source_->query_residency(il, gathered_.data(), (int) gathered_.size(), pending_.residency.data());
        }

        source_->load_layer(il, gathered_.data(), (int) gathered_.size());

        // Temporal prefetch: hint the next K layers with what the PREVIOUS token routed there,
        // to be read on idle lanes while this layer computes; then record this layer's routing
        // (the last token's row during prefill) for the next token to predict from.
        if (prefetch_layers_ > 0 && il >= 0 && il < n_layer_) {
            for (int k = 1; k <= prefetch_layers_; ++k) {
                const int tl = il + k;
                if (tl < n_layer_ && !prev_ids_[tl].empty())
                    source_->prefetch(tl, prev_ids_[tl].data(), (int) prev_ids_[tl].size());
            }
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
