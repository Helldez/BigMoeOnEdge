#include "bmoe/recipe.h"

#include <cstring>

namespace bmoe {

// The registry. v1 ships Qwen3 MoE (Qwen3-30B-A3B and siblings). Most llama.cpp MoE
// models are built by the same build_moe_ffn helper and expose the identical
// `ffn_{gate,up,down}_exps` naming, so adding one is usually a single row here — see
// docs/adding-a-model.md. Models that pack gate+up into one tensor (a merged
// `ffn_gate_up_exps`) need a distinct recipe shape and are intentionally not claimed
// by these rows.
static const MoeRecipe k_recipes[] = {
    {"qwen3moe", "ffn_gate_exps", "ffn_up_exps", "ffn_down_exps"},
    {"qwen2moe", "ffn_gate_exps", "ffn_up_exps", "ffn_down_exps"},
    // llada-moe is a diffusion MoE; the expert layout is standard, so expert streaming
    // applies mechanically. Note that its diffusion inference does not have the n=1
    // routing sparsity autoregressive decode relies on — see docs/limitations.md.
    {"llada-moe", "ffn_gate_exps", "ffn_up_exps", "ffn_down_exps"},
};

static const int k_n_recipes = (int) (sizeof(k_recipes) / sizeof(k_recipes[0]));

const MoeRecipe * find_moe_recipe(const char * arch) {
    if (!arch) {
        return nullptr;
    }
    for (int i = 0; i < k_n_recipes; ++i) {
        if (std::strcmp(k_recipes[i].arch, arch) == 0) {
            return &k_recipes[i];
        }
    }
    return nullptr;
}

int n_moe_recipes() {
    return k_n_recipes;
}
const MoeRecipe * moe_recipe_at(int i) {
    return (i >= 0 && i < k_n_recipes) ? &k_recipes[i] : nullptr;
}

} // namespace bmoe
