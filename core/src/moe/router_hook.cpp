#include "router_hook.h"

#include "ggml.h"

#include <cstdio>
#include <cstring>

namespace bmoe {

RouterHook::RouterHook(const MoeRecipe & recipe, int n_layer) : recipe_(recipe), n_layer_(n_layer) {
    captured_.assign(n_layer_ > 0 ? n_layer_ : 0, LayerExperts{});
}

// Match a tensor name of the form "blk.<il>.<suffix>.weight" against the recipe's three
// expert projections. Returns the projection index (0=gate,1=up,2=down) and layer, or -1.
static int match_expert(const char * name, const MoeRecipe & r, int & il_out) {
    int il = -1;
    int consumed = 0;
    if (std::sscanf(name, "blk.%d.%n", &il, &consumed) != 1 || il < 0) return -1;
    const char * rest = name + consumed; // "<suffix>.weight"
    const char * suffixes[3] = {r.gate_exps_suffix, r.up_exps_suffix, r.down_exps_suffix};
    for (int p = 0; p < 3; ++p) {
        const size_t sl = std::strlen(suffixes[p]);
        if (std::strncmp(rest, suffixes[p], sl) == 0 && std::strcmp(rest + sl, ".weight") == 0) {
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
    }
    return true;
}

} // namespace bmoe
