// Unit tests for the expert layout (bmoe/expert_layout.h): which tensors of a file are its routed
// experts, read from their shapes and the expert count the file declares.
//
// This replaced a table of architectures, so what it must prove is that the file alone decides:
// every layout the gates run (split, fused, gate-less), the companions that share an expert's name
// but not its rank, the blocks that carry no experts, and the files it must refuse - a dense model,
// a MoE whose layers disagree, one with more expert tensors per layer than the streamer holds.
// Pure: no model, no llama.cpp.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "bmoe/expert_layout.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace bmoe;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    if (!ok) ++failures;
    std::printf("%-78s %s\n", what.c_str(), ok ? "ok" : "FAIL");
}

static std::string blk(int il, const char * rest) {
    return "blk." + std::to_string(il) + "." + rest;
}

// A block of a file whose experts come in the split layout, with everything that sits beside them.
static void split_block(std::vector<TensorExtent> & t, int il, uint64_t n_expert) {
    t.push_back({blk(il, "attn_q.weight"), 0});
    t.push_back({blk(il, "ffn_gate_inp.weight"), 0}); // the router: 2-D
    t.push_back({blk(il, "exp_probs_b.bias"), 0});    // its bias: 1-D
    t.push_back({blk(il, "ffn_gate_exps.weight"), n_expert});
    t.push_back({blk(il, "ffn_up_exps.weight"), n_expert});
    t.push_back({blk(il, "ffn_down_exps.weight"), n_expert});
    t.push_back({blk(il, "ffn_down_exps.scale"), 0}); // a per-expert companion: 1-D
    t.push_back({blk(il, "ffn_up_shexp.weight"), 0}); // the shared expert: 2-D
}

int main() {
    const int n_expert = 8;

    // ── the split layout, with its companions ───────────────────────────────────────
    {
        std::vector<TensorExtent> t = {{"token_embd.weight", 0}, {"output.weight", 0}};
        for (int il = 0; il < 4; ++il)
            split_block(t, il, n_expert);
        const ExpertLayout L = discover_expert_layout(t, n_expert);
        check(L.ok, "split: a file with gate, up and down per layer is streamable");
        check(L.n_slots == 3, "split: three slots");
        check(L.suffix[0] == "ffn_down_exps" && L.suffix[1] == "ffn_gate_exps" && L.suffix[2] == "ffn_up_exps",
              "split: the slots are the suffixes, sorted");
        check(L.layers == std::vector<int>{0, 1, 2, 3}, "split: every block is a MoE layer");
        int il = -1;
        check(L.slot_of("blk.3.ffn_up_exps.weight", il) == 2 && il == 3, "slot_of: a projection's slot and layer");
        check(L.slot_of("blk.3.ffn_down_exps.scale", il) == -1, "slot_of: a companion scale stays resident");
        check(L.slot_of("blk.3.ffn_up_shexp.weight", il) == -1, "slot_of: a shared expert is not a routed one");
        check(L.slot_of("blk.3.ffn_gate_inp.weight", il) == -1, "slot_of: the router is not an expert");
        check(L.slot_of("blk.3.ffn_up_exps.weightx", il) == -1, "slot_of: the .weight tail is exact");
        check(L.slot_of("ffn_up_exps.weight", il) == -1, "slot_of: outside a block");
        check(L.slot_of("blk.9.ffn_up_exps.weight", il) == 2 && il == 9,
              "slot_of: answers by name, for a layer the file did not list (the MTP block)");
    }

    // ── the fused layout: two slots, the fused tensor a slot like any other ─────────
    {
        std::vector<TensorExtent> t;
        for (int il = 0; il < 3; ++il) {
            t.push_back({blk(il, "ffn_gate_up_exps.weight"), n_expert});
            t.push_back({blk(il, "ffn_down_exps.weight"), n_expert});
            t.push_back({blk(il, "ffn_gate.weight"), 0}); // the layer's dense shared expert
        }
        const ExpertLayout L = discover_expert_layout(t, n_expert);
        check(L.ok && L.n_slots == 2, "fused: two slots");
        check(L.suffix[0] == "ffn_down_exps" && L.suffix[1] == "ffn_gate_up_exps", "fused: the two suffixes");
        int il = -1;
        check(L.slot_of("blk.1.ffn_gate_up_exps.weight", il) == 1 && il == 1, "fused: the fused tensor's slot");
    }

    // ── the gate-less layout, behind leading dense blocks and beside non-MoE blocks ─
    {
        std::vector<TensorExtent> t;
        for (int il = 0; il < 8; ++il) {
            if (il < 2 || il % 3 == 0) { // leading dense blocks, then a recurrent block every third
                t.push_back({blk(il, "ssm_in.weight"), 0});
                continue;
            }
            t.push_back({blk(il, "ffn_up_exps.weight"), n_expert});
            t.push_back({blk(il, "ffn_down_exps.weight"), n_expert});
        }
        const ExpertLayout L = discover_expert_layout(t, n_expert);
        check(L.ok && L.n_slots == 2, "gate-less: two slots");
        check(L.layers == std::vector<int>{2, 4, 5, 7}, "gate-less: only the blocks with experts are MoE layers");
    }

    // ── a tensor indexed by another count is not one of these experts ──────────────
    {
        std::vector<TensorExtent> t;
        for (int il = 0; il < 2; ++il) {
            split_block(t, il, n_expert);
            t.push_back({blk(il, "ffn_up_chexps.weight"), 4}); // routed, but over its own count
        }
        const ExpertLayout L = discover_expert_layout(t, n_expert);
        check(L.ok && L.n_slots == 3, "another expert count: ignored, the layer keeps its three slots");
    }

    // ── refusals, each with its reason ──────────────────────────────────────────────
    {
        std::vector<TensorExtent> t = {
            {"token_embd.weight", 0}, {"blk.0.ffn_up.weight", 0}, {"blk.0.ffn_down.weight", 0}};
        const ExpertLayout L = discover_expert_layout(t, 0);
        check(!L.ok && L.error.find("dense") != std::string::npos, "refused: a dense model (expert_count 0)");
        check(!discover_expert_layout(t, 1).ok, "refused: an expert count of one");
        int il = -1;
        check(L.slot_of("blk.0.ffn_up.weight", il) == -1, "a refused layout claims no tensor");
    }
    {
        std::vector<TensorExtent> t = {{"blk.0.ffn_up.weight", 0}, {"blk.0.ffn_gate_inp.weight", 0}};
        const ExpertLayout L = discover_expert_layout(t, n_expert);
        check(!L.ok && L.error.find("no routed expert tensor") != std::string::npos,
              "refused: the count is declared but no 3-D weight is indexed by it");
    }
    {
        std::vector<TensorExtent> t;
        split_block(t, 0, n_expert);
        t.push_back({blk(1, "ffn_up_exps.weight"), n_expert}); // layer 1 lacks gate and down
        const ExpertLayout L = discover_expert_layout(t, n_expert);
        check(!L.ok && L.error.find("layer 1") != std::string::npos && L.error.find("disagree") != std::string::npos,
              "refused: the layers disagree on their expert tensors, and the reason names the layer");
    }
    {
        std::vector<TensorExtent> t;
        split_block(t, 0, n_expert);
        t.push_back({blk(0, "ffn_extra_exps.weight"), n_expert});
        const ExpertLayout L = discover_expert_layout(t, n_expert);
        check(!L.ok && L.error.find("more than") != std::string::npos,
              "refused: four expert tensors in a layer is more than the streamer holds");
    }

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall expert-layout checks passed\n");
    return 0;
}
