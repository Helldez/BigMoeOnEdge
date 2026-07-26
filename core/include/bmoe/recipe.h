// MoE architecture recipes.
//
// A recipe is the ONLY architecture-specific knowledge in the engine: the gguf tensor
// name suffixes of a layer's expert weight tensors. Everything else — routing, the
// number of experts, the per-expert byte stride — is discovered at runtime from the
// tensors themselves. Supporting a new MoE family is therefore one table row plus a
// byte-identity gate run, never a code change in the streaming path.
//
// This header is pure policy: it has no llama.cpp dependency and compiles standalone.
#pragma once

#include <string>

namespace bmoe {

// The expert weight tensors of a MoE layer, named `blk.<il>.<suffix>.weight` in the gguf.
// A recipe lists the per-layer expert tensors as a suffix table: the common split layout
// names three ({gate, up, down} projections), while architectures that fuse gate+up into
// one tensor name two ({gate_up, down}). Unused slots are nullptr. Each named tensor is
// 3-D and its dim-2 indexes the expert (ne[2] == n_expert); the engine streams whatever
// the recipe names and never needs to know which projection a given tensor carries — a
// fused gate_up is just an expert tensor with a larger per-expert stride, discovered at
// runtime like any other.
struct MoeRecipe {
    static constexpr int max_exps = 3;
    const char * arch;                  // gguf general.architecture, e.g. "qwen3moe"
    const char * exps_suffix[max_exps]; // e.g. {"ffn_gate_exps", "ffn_up_exps", "ffn_down_exps"}
};

// Look up a recipe by gguf architecture string. Returns nullptr if the architecture is
// not in the registry (the engine then refuses to stream rather than guess).
const MoeRecipe * find_moe_recipe(const char * arch);

// Number of registered recipes and indexed access, for `--list-archs` and tests.
int n_moe_recipes();
const MoeRecipe * moe_recipe_at(int i);

// A regular expression matching exactly this recipe's expert tensors, e.g.
// `\.(ffn_gate_exps|ffn_up_exps|ffn_down_exps)\.` — the form llama.cpp's tensor buffer-type
// overrides expect (an unanchored std::regex search over the full gguf tensor name).
//
// Derived from the suffix table rather than written out, so a recipe row added for a new
// architecture pins its own experts to the CPU with no second place to update. The escaped dots
// bound the suffix so it cannot match a longer name that merely contains it.
//
// Note this matches the per-expert companions too (`ffn_down_exps.scale` and friends), unlike the
// streamer's own tensor matching, which requires an exact `.weight` tail. The two ask different
// questions and correctly give different answers: the streamer asks "do I read this from flash?"
// (no — a scale is small and resident), while placement asks "which device computes with this?"
// (the same one holding the weights, or the layer's FFN would copy across devices every token).
std::string expert_tensor_pattern(const MoeRecipe & recipe);

} // namespace bmoe
