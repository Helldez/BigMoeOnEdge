#include "router_hook.h"

#include "ggml.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>

namespace bmoe {

RouterHook::RouterHook(const MoeRecipe & recipe, int n_layer) : recipe_(recipe), n_layer_(n_layer) {
    const int n = n_layer_ > 0 ? n_layer_ : 0;
    captured_.assign(n, LayerExperts{});
    prev_ids_.assign(n, std::vector<int32_t>{});
    gate_w_.assign(n, nullptr);
    gate_s_.assign(n, nullptr);
    next_moe_layer_.assign(n, -1);
    spec_pred_.assign(n, std::vector<int32_t>{});
}

void RouterHook::set_spec_gate(bool on, int n_expert_used, float rms_eps) {
    spec_gate_ = on;
    n_expert_used_ = n_expert_used;
    if (rms_eps > 0.0f) rms_eps_ = rms_eps;
}

// Match "blk.<il>.<suffix><tail>" (e.g. suffix "ffn_gate_inp", tail ".weight"). Returns the layer.
static bool match_named(const char * name, const char * suffix, const char * tail, int n_layer, int & il_out) {
    if (!suffix) return false;
    int il = -1, consumed = 0;
    if (std::sscanf(name, "blk.%d.%n", &il, &consumed) != 1 || il < 0 || il >= n_layer) return false;
    const char * rest = name + consumed;
    const size_t sl = std::strlen(suffix);
    return std::strncmp(rest, suffix, sl) == 0 && std::strcmp(rest + sl, tail) == 0 && (il_out = il, true);
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

void RouterHook::begin_capture() {
    capturing_ = true;
    for (auto & L : captured_)
        L = LayerExperts{};
    std::fill(gate_w_.begin(), gate_w_.end(), nullptr);
    std::fill(gate_s_.begin(), gate_s_.end(), nullptr);
}
void RouterHook::end_capture() {
    capturing_ = false;
    // Precompute, for each layer, the next bound MoE layer after it — the layer whose router a
    // speculative prediction at this layer targets. Skips dense layers (e.g. Gemma's interleave).
    int next = -1;
    for (int il = n_layer_ - 1; il >= 0; --il) {
        next_moe_layer_[il] = next;
        if (captured_[il].bound) next = il;
    }
}

bool RouterHook::c_eval(ggml_tensor * t, bool ask, void * user_data) {
    return static_cast<RouterHook *>(user_data)->on_eval(t, ask);
}

bool RouterHook::on_eval(ggml_tensor * t, bool ask) {
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
            // Also harvest the mmap-resident router weights for speculative gating (if wired).
            if (recipe_.router_suffix) {
                for (int s = 0; s < GGML_MAX_SRC; ++s) {
                    ggml_tensor * src = t->src[s];
                    if (!src || src->name[0] == '\0') continue;
                    int il = -1;
                    if (match_named(src->name, recipe_.router_suffix, ".weight", n_layer_, il)) gate_w_[il] = src;
                    if (match_named(src->name, recipe_.router_scale_suffix, ".scale", n_layer_, il)) gate_s_[il] = src;
                }
            }
        }
        return false; // capture never isolates a node
    }

    // ── stream: routing nodes get the single-node barrier; so does the router-input node when
    //    speculative gating is on (we read its value to predict the next layer's experts) ──
    int il = -1;
    const bool is_topk = std::sscanf(t->name, "ffn_moe_topk-%d", &il) == 1 && il >= 0;
    int ril = -1;
    const bool is_router_in = spec_gate_ && !spec_disabled_ && recipe_.router_input_fmt &&
                              std::sscanf(t->name, recipe_.router_input_fmt, &ril) == 1 && ril >= 0 && ril < n_layer_;
    if (ask) return is_topk || is_router_in;

    if (source_ && is_router_in && t->data) predict_and_prefetch(t, ril);

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
        source_->load_layer(il, gathered_.data(), (int) gathered_.size());

        // Score a speculative-gating prediction for this layer against the routing that actually
        // happened (decode only, n=1: gathered_ is exactly this token's top-k). Recall = fraction
        // of predicted experts the router really selected.
        if (spec_gate_ && nt == 1 && il >= 0 && il < n_layer_ && !spec_pred_[il].empty()) {
            long long hit = 0;
            for (int32_t a : gathered_)
                if (std::find(spec_pred_[il].begin(), spec_pred_[il].end(), a) != spec_pred_[il].end()) ++hit;
            spec_pred_hit_ += hit;
            spec_pred_total_ += (long long) spec_pred_[il].size();
            spec_pred_[il].clear();
        }

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

// Predict the NEXT MoE layer's routing by running its gate on THIS layer's hidden state (the
// residual stream changes slowly between layers), and prefetch the top-k. Only the ids matter —
// they feed the byte-safe prefetch queue — so approximations here cost a wasted read at worst.
void RouterHook::predict_and_prefetch(const ggml_tensor * node, int il) {
    const int target = next_moe_layer_[il];
    if (target < 0) return;
    const ggml_tensor * gw = gate_w_[target];
    if (!gw || node->type != GGML_TYPE_F32) return;
    if (gw->type != GGML_TYPE_F32 && gw->type != GGML_TYPE_F16) {
        if (!spec_disabled_) {
            spec_disabled_ = true;
            std::fprintf(stderr, "bmoe: speculative gating disabled — router weight is quantized\n");
        }
        return;
    }

    const int n_embd = (int) node->ne[0];
    const int nt = (int) node->ne[1];
    if (n_embd <= 0 || (int) gw->ne[0] != n_embd) return;
    const int j = nt - 1; // last token's column

    // Read the hidden state, applying the architecture's router pre-transform.
    spec_hidden_.resize(n_embd);
    for (int d = 0; d < n_embd; ++d)
        spec_hidden_[d] =
            *(const float *) ((const char *) node->data + (size_t) j * node->nb[1] + (size_t) d * node->nb[0]);
    if (recipe_.router_pre == RouterPre::kRmsScaled) {
        double ss = 0.0;
        for (int d = 0; d < n_embd; ++d)
            ss += (double) spec_hidden_[d] * spec_hidden_[d];
        const float inv_rms = 1.0f / std::sqrt((float) (ss / n_embd) + rms_eps_);
        const float inv_sqrt_d = 1.0f / std::sqrt((float) n_embd);
        const ggml_tensor * sc = gate_s_[target];
        for (int d = 0; d < n_embd; ++d) {
            const float s = sc ? *(const float *) ((const char *) sc->data + (size_t) d * sc->nb[0]) : 1.0f;
            spec_hidden_[d] = spec_hidden_[d] * inv_rms * inv_sqrt_d * s;
        }
    }

    // logits[e] = dot(gate_row_e, hidden). Gate is [n_embd, n_expert], row-major per expert.
    const int n_expert = (int) gw->ne[1];
    if (n_expert <= 0) return;
    spec_logits_.resize(n_expert);
    for (int e = 0; e < n_expert; ++e) {
        const char * row = (const char *) gw->data + (size_t) e * gw->nb[1];
        double acc = 0.0;
        if (gw->type == GGML_TYPE_F32) {
            const float * r = (const float *) row;
            for (int d = 0; d < n_embd; ++d)
                acc += (double) r[d] * spec_hidden_[d];
        } else {
            const ggml_fp16_t * r = (const ggml_fp16_t *) row;
            for (int d = 0; d < n_embd; ++d)
                acc += (double) ggml_fp16_to_fp32(r[d]) * spec_hidden_[d];
        }
        spec_logits_[e] = (float) acc;
    }

    // Top-k experts by logit (exact weights are irrelevant — only the ids feed prefetch).
    const int k = std::min(n_expert_used_ > 0 ? n_expert_used_ : 1, n_expert);
    std::vector<int32_t> idx(n_expert);
    std::iota(idx.begin(), idx.end(), 0);
    std::partial_sort(idx.begin(), idx.begin() + k, idx.end(),
                      [&](int32_t a, int32_t b) { return spec_logits_[a] > spec_logits_[b]; });
    idx.resize(k);
    source_->prefetch(target, idx.data(), k);
    spec_pred_[target] = idx; // scored against the actual routing when the target layer runs
}

} // namespace bmoe
