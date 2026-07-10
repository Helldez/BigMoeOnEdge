// MoE architecture recipes.
//
// A recipe is the ONLY architecture-specific knowledge in the engine: the gguf tensor
// name suffixes of a layer's three expert projections. Everything else — routing, the
// number of experts, the per-expert byte stride — is discovered at runtime from the
// tensors themselves. Supporting a new MoE family is therefore one table row plus a
// byte-identity gate run, never a code change in the streaming path.
//
// This header is pure policy: it has no llama.cpp dependency and compiles standalone.
#pragma once

namespace bmoe {

// The three expert weight tensors of a MoE layer, named `blk.<il>.<suffix>.weight` in
// the gguf. Each is a 3-D tensor whose dim-2 indexes the expert (ne[2] == n_expert).
struct MoeRecipe {
    const char * arch;              // gguf general.architecture, e.g. "qwen3moe"
    const char * gate_exps_suffix;  // e.g. "ffn_gate_exps"
    const char * up_exps_suffix;    // e.g. "ffn_up_exps"
    const char * down_exps_suffix;  // e.g. "ffn_down_exps"
};

// Look up a recipe by gguf architecture string. Returns nullptr if the architecture is
// not in the registry (the engine then refuses to stream rather than guess).
const MoeRecipe * find_moe_recipe(const char * arch);

// Number of registered recipes and indexed access, for `--list-archs` and tests.
int                n_moe_recipes();
const MoeRecipe *  moe_recipe_at(int i);

} // namespace bmoe
