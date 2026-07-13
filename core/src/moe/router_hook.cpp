#include "router_hook.h"

#include "ggml.h"

#include <cstdio>
#include <cstring>

namespace bmoe {

RouterHook::RouterHook(const MoeRecipe & recipe, int n_layer) : recipe_(recipe), n_layer_(n_layer) {
    captured_.assign(n_layer_ > 0 ? n_layer_ : 0, LayerExperts{});
    prev_ids_.assign(n_layer_ > 0 ? n_layer_ : 0, std::vector<int32_t>{});
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
}
void RouterHook::end_capture() {
    capturing_ = false;
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
        }
        return false; // capture never isolates a node
    }

    // ── stream: only the routing nodes get the single-node barrier ──
    int il = -1;
    const bool is_topk = std::sscanf(t->name, "ffn_moe_topk-%d", &il) == 1 && il >= 0;
    if (ask) return is_topk;
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
                rec.push_back(*(const int32_t *) ((const char *) t->data + (size_t) last * t->nb[1] +
                                                  (size_t) k * t->nb[0]));
        }
    }
    return true;
}

} // namespace bmoe
