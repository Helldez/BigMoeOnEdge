#include "bmoe/recipe.h"

#include <cstring>

namespace bmoe {

// The registry. Ships Qwen3 MoE (Qwen3-30B-A3B and siblings). Most llama.cpp MoE models
// are built by the same build_moe_ffn helper and expose the identical
// `ffn_{gate,up,down}_exps` naming, so adding one is usually a single row here — see
// docs/adding-a-model.md. Models that fuse gate+up into one tensor (a merged
// `ffn_gate_up_exps`) name two expert tensors instead of three; that is still one row,
// with the fused suffix in the first slot and a nullptr tail.
static const MoeRecipe k_recipes[] = {
    // Speculative-gating fields: qwen3moe/qwen2moe route on the post-norm hidden state
    // (node "ffn_norm-<il>"), so the observed node is the router input directly (kNone).
    {"qwen3moe",
     {"ffn_gate_exps", "ffn_up_exps", "ffn_down_exps"},
     "ffn_norm-%d",
     "ffn_gate_inp",
     nullptr,
     RouterPre::kNone},
    {"qwen2moe",
     {"ffn_gate_exps", "ffn_up_exps", "ffn_down_exps"},
     "ffn_norm-%d",
     "ffn_gate_inp",
     nullptr,
     RouterPre::kNone},
    // qwen35moe (Qwen3.5 MoE, e.g. 35B-A3B) is a hybrid attention/SSM stack: some layers run
    // full attention, others a Mamba-style SSM block, but every MoE layer names its experts
    // with the standard split suffixes, so streaming is one row. There is also an always-on
    // shared expert (ffn_*_shexp) that stays mmap-resident and lowers the streamed fraction.
    // Speculative gating is left unwired (router_input_fmt=nullptr): the observed router-input
    // node is not uniform across the hybrid layer types, so --spec-gate refuses here rather
    // than guessing. Basic streaming and the auto cache are unaffected.
    {"qwen35moe", {"ffn_gate_exps", "ffn_up_exps", "ffn_down_exps"}, nullptr, nullptr, nullptr, RouterPre::kNone},
    // gemma4 (Gemma 4 MoE, e.g. 26B-A4B) fuses gate+up into blk.<il>.ffn_gate_up_exps —
    // to the streamer just an expert tensor with a 2x per-expert stride. The per-expert
    // ffn_down_exps.scale, the router (ffn_gate_inp.{weight,scale}) and the always-on
    // shared expert (the layer's dense ffn_{gate,up,down}) match no suffix and stay mmap-
    // resident; the resident shared expert lowers the streamed fraction — see
    // docs/limitations.md.
    // Gemma's router runs on the raw attn_out ("attn_out-<il>") with an explicit
    // rms_norm * 1/sqrt(n_embd) * per-channel-scale (ffn_gate_inp.scale) before the gate.
    {"gemma4",
     {"ffn_gate_up_exps", "ffn_down_exps", nullptr},
     "attn_out-%d",
     "ffn_gate_inp",
     "ffn_gate_inp", // the ".scale" tail is added by the capture matcher
     RouterPre::kRmsScaled},
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
