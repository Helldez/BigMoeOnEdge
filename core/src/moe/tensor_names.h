// llama.cpp's tensor and graph-node names, read in one place.
//
// The engine finds everything it touches by name: a block's weights are "blk.<il>.<rest>", an
// expert weight is "blk.<il>.<recipe suffix>.weight", a per-layer graph node is "<name>-<il>", and
// the capacity fitter hands back its placement as regex patterns over those same names. Those are
// the format's and the graph builder's conventions, shared by every architecture llama.cpp
// converts - not a model's - and each was once parsed where it was used, with a sscanf here, a
// strncmp there and a std::regex in the planner. A rename upstream is now one file to change, and
// every reader parses the same way: digits only, the whole index, nothing after it.
//
// Which tensors are EXPERTS stays the recipe's to say (bmoe/recipe.h); this only knows how a name
// is spelled. Pure: no llama.cpp, no ggml, no allocation on the per-node paths.
#pragma once

#include "bmoe/recipe.h"

#include <cstring>
#include <string>
#include <string_view>

namespace bmoe::tensor_names {

// ── tensors ─────────────────────────────────────────────────────────────────────────

// "blk.<il>.<rest>" -> il, with `rest` (when given) pointing just past the dot after the index.
// -1 for a name outside any block - the token table, the output head, the final norm - and for
// one whose index is not all digits.
int block_layer(const char * name, const char ** rest = nullptr);

// The recipe slot (its position in exps_suffix) whose expert weight tensor `name` is, with its
// layer in `il_out`, or -1. Exactly "blk.<il>.<suffix>.weight": the ".weight" tail is load-bearing,
// since a companion tensor like "ffn_down_exps.scale" prefix-matches its suffix and must stay a
// mapped, resident tensor rather than a streamed one.
int expert_slot(const char * name, const MoeRecipe & recipe, int & il_out);

// ── graph nodes ─────────────────────────────────────────────────────────────────────

// The per-layer nodes the engine asks the graph for, as build_moe_ffn and the layer builders name
// them: "<prefix><il>".
inline constexpr std::string_view k_topk_node = "ffn_moe_topk-";
inline constexpr std::string_view k_logits_node = "ffn_moe_logits-";
inline constexpr std::string_view k_layer_out_node = "l_out-";

// Every routing node - the topk, the weight chain, the gate matmul - starts with this. A graph has
// thousands of nodes and the router hook's ask pass sees all of them, so this one comparison is
// what keeps the rest of the matching off the hot path.
inline constexpr std::string_view k_moe_node_prefix = "ffn_moe_";

inline bool is_moe_node(const char * name) {
    return std::strncmp(name, k_moe_node_prefix.data(), k_moe_node_prefix.size()) == 0;
}

// The "<il>" after `prefix`, digits only to the end of the string, or -1 if `name` is not
// "<prefix><il>". Stricter than a "%d", which would read "12abc" as layer 12.
int node_layer(const char * name, std::string_view prefix);

// The layer of any per-layer node: the digits after the last '-'. Anything else (embeddings, the
// output head, the KQ mask) belongs to no layer and is -1. Generic on purpose: a trace must not
// carry a table of which nodes an architecture emits.
int any_node_layer(const char * name);

// The router-weight chain build_moe_ffn emits per layer: ffn_moe_weights -> (_softmax | _norm) ->
// (_scaled), each an optional refinement of the one before, so the LAST one a layer offers carries
// the weight its expert outputs are scaled by. Last-wins keeps this architecture-independent: no
// table of which gating each model uses. The names are mutually exclusive, which is why a match is
// reported as an index into this list - a layer's terminal node is then remembered as an integer.
inline constexpr std::string_view k_weight_nodes[] = {
    "ffn_moe_weights",
    "ffn_moe_weights_softmax",
    "ffn_moe_weights_norm",
    "ffn_moe_weights_scaled",
};
inline constexpr int k_weight_node_count = (int) (sizeof(k_weight_nodes) / sizeof(k_weight_nodes[0]));

// Which of k_weight_nodes `name` is ("<node>-<il>"), with its layer in `il_out`, or -1. The '-' is
// load-bearing: it rejects "ffn_moe_weights_sum-3", a row sum that prefix-matches the first name.
int weight_node(const char * name, int & il_out);

// ── tensor-placement patterns ───────────────────────────────────────────────────────

// The pattern llama.cpp's capacity fitter writes to keep one layer's routed experts in host memory
// (common/fit.cpp, its MoE layer fraction). It is a regex for llama.cpp's loader to apply; here it
// is only ever spelled and recognised, as text.
std::string expert_override_pattern(int il);

// The layer an expert-override pattern of that form names, or -1 for any other pattern - a whole
// layer, an attention share, the all-layers form with "\d+" in place of an index.
int expert_override_layer(const char * pattern);

} // namespace bmoe::tensor_names
