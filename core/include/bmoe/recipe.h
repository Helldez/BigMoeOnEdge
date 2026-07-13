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

namespace bmoe {

// The expert weight tensors of a MoE layer, named `blk.<il>.<suffix>.weight` in the gguf.
// A recipe lists the per-layer expert tensors as a suffix table: the common split layout
// names three ({gate, up, down} projections), while architectures that fuse gate+up into
// one tensor name two ({gate_up, down}). Unused slots are nullptr. Each named tensor is
// 3-D and its dim-2 indexes the expert (ne[2] == n_expert); the engine streams whatever
// the recipe names and never needs to know which projection a given tensor carries — a
// fused gate_up is just an expert tensor with a larger per-expert stride, discovered at
// runtime like any other.
// How the router's input is derived from the observed graph node, before the gate matmul.
//   kNone      — the observed node IS the router input (a post-norm hidden state).
//   kRmsScaled — rms_norm(node) * (1/sqrt(n_embd)) * per-channel scale (Gemma 4's router, which
//                runs on the raw attn_out with an explicit normalise-and-scale in the graph).
enum class RouterPre { kNone, kRmsScaled };

struct MoeRecipe {
    static constexpr int max_exps = 3;
    const char * arch;                  // gguf general.architecture, e.g. "qwen3moe"
    const char * exps_suffix[max_exps]; // e.g. {"ffn_gate_exps", "ffn_up_exps", "ffn_down_exps"}

    // Speculative gating (optional): predict the NEXT MoE layer's experts by running its router on
    // the current layer's hidden state (which changes slowly along the residual stream). Only the
    // top-k ids are needed — they feed the prefetch hint — so a mispredict just wastes a read and
    // never affects output. router_input_fmt == nullptr means the architecture is not wired for
    // speculative gating (the feature then refuses rather than guesses).
    const char * router_input_fmt = nullptr;    // graph node to observe, e.g. "ffn_norm-%d"
    const char * router_suffix = nullptr;       // mmap-resident gate weight suffix ("ffn_gate_inp")
    const char * router_scale_suffix = nullptr; // per-channel scale suffix, or nullptr
    RouterPre router_pre = RouterPre::kNone;
};

// Look up a recipe by gguf architecture string. Returns nullptr if the architecture is
// not in the registry (the engine then refuses to stream rather than guess).
const MoeRecipe * find_moe_recipe(const char * arch);

// Number of registered recipes and indexed access, for `--list-archs` and tests.
int n_moe_recipes();
const MoeRecipe * moe_recipe_at(int i);

} // namespace bmoe
