// Unit tests for tensor_names (core/src/moe/tensor_names.h): how the engine reads llama.cpp's tensor
// and graph-node names, and the capacity fitter's tensor-placement patterns.
//
// Every reader of a name goes through this module - the router hook on every graph node, the model
// probe on every tensor of the file, the placement probe on what the fitter hands back - so a name
// it misreads is a tensor streamed that should be resident, or a layer planned on the wrong side of
// a device boundary. The cases are the shapes the conventions really produce plus the near misses
// each rule exists to reject. Pure: no model, no llama.cpp.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "tensor_names.h"

#include <cstdio>
#include <string>

using namespace bmoe;
namespace tn = bmoe::tensor_names;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    if (!ok) ++failures;
    std::printf("%-72s %s\n", what.c_str(), ok ? "ok" : "FAIL");
}

int main() {
    // ── blocks ───────────────────────────────────────────────────────────────────────
    {
        const char * rest = nullptr;
        check(tn::block_layer("blk.12.attn_q.weight", &rest) == 12 && std::string(rest) == "attn_q.weight",
              "block_layer: index and the rest of the name");
        check(tn::block_layer("blk.0.ffn_down_exps.scale") == 0,
              "block_layer: any tensor of a block, not only .weight");
        check(tn::block_layer("token_embd.weight") == -1, "block_layer: a top-level tensor is in no block");
        check(tn::block_layer("output.weight") == -1, "block_layer: the output head is in no block");
        check(tn::block_layer("blk.x.attn_q.weight") == -1, "block_layer: rejects a non-numeric index");
        check(tn::block_layer("blk.-1.attn_q.weight") == -1, "block_layer: rejects a signed index");
        check(tn::block_layer("blk..attn_q.weight") == -1, "block_layer: rejects an empty index");
        check(tn::block_layer("blk.12") == -1, "block_layer: rejects an index with nothing after it");
        check(tn::block_layer("blk.12x.attn_q.weight") == -1, "block_layer: rejects an index with a tail");
    }

    // ── expert tensors, by the recipe ────────────────────────────────────────────────
    {
        const MoeRecipe * split = find_moe_recipe("qwen3moe");
        const MoeRecipe * fused = find_moe_recipe("gemma4");
        check(split && fused, "recipes for a split and a fused layout exist");
        if (split && fused) {
            int il = -1;
            check(tn::expert_slot("blk.3.ffn_up_exps.weight", *split, il) == 1 && il == 3,
                  "expert_slot: a split projection's slot and layer");
            il = -1;
            check(tn::expert_slot("blk.0.ffn_gate_up_exps.weight", *fused, il) == 0 && il == 0,
                  "expert_slot: a fused projection");
            check(tn::expert_slot("blk.3.ffn_down_exps.scale", *split, il) == -1,
                  "expert_slot: a companion scale stays resident");
            check(tn::expert_slot("blk.3.ffn_up_exps.weightx", *split, il) == -1,
                  "expert_slot: the .weight tail is exact");
            check(tn::expert_slot("blk.3.ffn_up_shexp.weight", *split, il) == -1,
                  "expert_slot: a shared expert is not a routed one");
            check(tn::expert_slot("blk.3.ffn_gate_up_exps.weight", *split, il) == -1,
                  "expert_slot: a suffix the recipe does not name");
            check(tn::expert_slot("ffn_up_exps.weight", *split, il) == -1, "expert_slot: outside a block");
        }
    }

    // ── graph nodes ──────────────────────────────────────────────────────────────────
    {
        check(tn::is_moe_node("ffn_moe_topk-7") && !tn::is_moe_node("attn_norm-7"), "is_moe_node: by prefix");
        check(tn::node_layer("ffn_moe_topk-7", tn::k_topk_node) == 7, "node_layer: the topk node's layer");
        check(tn::node_layer("ffn_moe_logits-40", tn::k_logits_node) == 40, "node_layer: the gate matmul");
        check(tn::node_layer("l_out-3", tn::k_layer_out_node) == 3, "node_layer: a layer's output");
        check(tn::node_layer("ffn_moe_topk-12abc", tn::k_topk_node) == -1, "node_layer: digits to the end only");
        check(tn::node_layer("ffn_moe_topk-", tn::k_topk_node) == -1, "node_layer: rejects an empty index");
        check(tn::node_layer("ffn_moe_topk-99999999", tn::k_topk_node) == -1, "node_layer: rejects a runaway index");
        check(tn::node_layer("ffn_moe_logits-3", tn::k_topk_node) == -1, "node_layer: another node's prefix");
        check(tn::any_node_layer("attn_norm-12") == 12, "any_node_layer: any per-layer node");
        check(tn::any_node_layer("result_norm") == -1, "any_node_layer: a node of no layer");
        check(tn::any_node_layer("node-") == -1, "any_node_layer: rejects an empty index");
        check(tn::any_node_layer("cache_k_l2 (view)") == -1, "any_node_layer: a view of no layer");
    }

    // ── the router weight chain ──────────────────────────────────────────────────────
    {
        int il = -1;
        check(tn::weight_node("ffn_moe_weights-3", il) == 0 && il == 3, "weight_node: the chain's first node");
        il = -1;
        check(tn::weight_node("ffn_moe_weights_norm-11", il) == 2 && il == 11, "weight_node: a refinement");
        il = -1;
        check(tn::weight_node("ffn_moe_weights_scaled-0", il) == 3 && il == 0, "weight_node: the last refinement");
        check(tn::weight_node("ffn_moe_weights_sum-3", il) == -1, "weight_node: a row sum is not a weight");
        check(tn::weight_node("ffn_moe_weights-3x", il) == -1, "weight_node: digits to the end only");
        check(tn::weight_node("ffn_moe_topk-3", il) == -1, "weight_node: not a weight node");
    }

    // ── the fitter's expert-override patterns ────────────────────────────────────────
    {
        // Spelled as common/fit.cpp spells its MoE layer fraction.
        check(tn::expert_override_layer("blk\\.5\\.ffn_(up|down|gate_up|gate)_(ch|)exps") == 5,
              "expert_override_layer: the fitter's per-layer pattern");
        check(tn::expert_override_layer(tn::expert_override_pattern(47).c_str()) == 47,
              "expert_override_layer: reads back what expert_override_pattern spells");
        check(tn::expert_override_layer("(blk\\.2\\.ffn_(up|down|gate_up|gate)_(ch|)exps)") == 2,
              "expert_override_layer: found inside a longer pattern");
        check(tn::expert_override_layer("blk\\.5\\.ffn_(gate|up|gate_up|down).*") == -1,
              "expert_override_layer: a whole layer's FFN is not the expert form");
        check(tn::expert_override_layer("blk\\.5\\.ffn_down.*") == -1, "expert_override_layer: a share of a layer");
        check(tn::expert_override_layer("blk\\.\\d+\\.ffn_(up|down|gate_up|gate)_(ch|)exps") == -1,
              "expert_override_layer: the all-layers form names no layer");
        check(tn::expert_override_layer("exps=CPU") == -1, "expert_override_layer: an unrelated pattern");
    }

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall tensor-name checks passed\n");
    return 0;
}
