// The model half of the planner's input: everything read from the gguf's own metadata and tensor
// shapes, with no tensor data touched and the model never loaded.
//
// Which tensors are experts comes from the architecture recipe — the one table in the engine that
// carries architecture knowledge — so a family the registry does not know produces a profile with
// no experts, and the planner declines rather than guessing at a layout.

#include "bmoe/probe.h"

#include "../moe/gguf_offsets.h"
#include "bmoe/recipe.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

namespace bmoe {

namespace {

// `blk.<il>.<suffix>.weight` -> (il, suffix), the naming every recipe row is written against.
// Returns false for anything else, which is every dense tensor.
bool split_block_tensor(const std::string & name, int & layer, std::string & suffix) {
    if (name.compare(0, 4, "blk.") != 0) return false;
    const size_t dot = name.find('.', 4);
    if (dot == std::string::npos) return false;
    const std::string num = name.substr(4, dot - 4);
    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) return false;
    layer = std::atoi(num.c_str());
    suffix = name.substr(dot + 1);
    // Trim the trailing ".weight"; a recipe names the tensor, not the field.
    const std::string tail = ".weight";
    if (suffix.size() > tail.size() && suffix.compare(suffix.size() - tail.size(), tail.size(), tail) == 0)
        suffix.resize(suffix.size() - tail.size());
    else
        return false; // scales, biases and probs are not the expert weight tensors
    return true;
}

bool recipe_names(const MoeRecipe & r, const std::string & suffix) {
    for (int i = 0; i < MoeRecipe::max_exps; ++i)
        if (r.exps_suffix[i] && suffix == r.exps_suffix[i]) return true;
    return false;
}

} // namespace

ModelProfile probe_model(const char * model_path) {
    ModelProfile m;
    if (!model_path || !*model_path) return m;

    const GgufMeta meta = read_gguf_meta(model_path);
    if (!meta.ok) return m;

    m.arch = meta.info.arch;
    m.n_expert = (uint32_t) std::max(0, meta.info.n_expert);
    m.n_expert_used = (uint32_t) std::max(0, meta.info.n_expert_used);
    m.ok = true;

    const MoeRecipe * recipe = find_moe_recipe(m.arch.c_str());

    // Per MoE layer: the sum over its expert tensors of ONE expert's bytes. Accumulated from the
    // tensors the file actually carries rather than multiplied out, because the per-projection
    // stride is not uniform — a fused gate_up is twice a split one — and an architecture with
    // leading dense blocks has layers that contribute nothing.
    std::map<int, uint64_t> per_layer_one_expert;
    std::map<int, uint64_t> per_layer_expert_bytes;
    uint32_t projections_seen = 0;
    std::map<int, uint32_t> projections_per_layer;
    std::set<int> mtp_layers; // blocks that carry a multi-token-prediction head

    bool saw_output_weight = false;
    bool saw_token_embd = false;

    for (const auto & kv : meta.offsets.size_by_name) {
        const std::string & name = kv.first;
        const uint64_t size = kv.second;
        m.file_bytes += size;

        if (name == "output.weight") saw_output_weight = true;
        if (name == "token_embd.weight") saw_token_embd = true;

        int layer = 0;
        std::string suffix;
        const bool in_block = split_block_tensor(name, layer, suffix);
        if (name.find("nextn") != std::string::npos) {
            m.has_mtp = true;
            // The MTP block lives at its own block index and names expert tensors like any other,
            // but llama.cpp does not load it by default — so those experts are never routed and
            // never streamed. Counting them would inflate the cache floor by a whole layer.
            if (in_block) mtp_layers.insert(layer);
        }

        const bool is_expert = recipe && m.n_expert > 0 && in_block && recipe_names(*recipe, suffix);
        if (is_expert) {
            const uint64_t one = size / m.n_expert;
            per_layer_expert_bytes[layer] += size;
            per_layer_one_expert[layer] += one;
            projections_per_layer[layer]++;
            if (m.expert_type_id < 0) {
                const auto t = meta.offsets.type_by_name.find(name);
                if (t != meta.offsets.type_by_name.end()) m.expert_type_id = t->second;
            }
        } else {
            m.largest_dense_tensor = std::max(m.largest_dense_tensor, size);
        }
    }

    // Everything the MTP block owns is dead weight for the streamer: drop its layer from every
    // expert total, so the cache floor and the cap describe what actually gets read.
    for (int il : mtp_layers) {
        per_layer_expert_bytes.erase(il);
        per_layer_one_expert.erase(il);
        projections_per_layer.erase(il);
    }

    for (const auto & kv : per_layer_expert_bytes)
        m.expert_bytes += kv.second;
    for (const auto & kv : per_layer_one_expert) {
        // The granule the streamer issues as a read is one expert of one projection; the largest
        // of them is what should be looked up on the storage rate curve.
        const uint32_t projs = projections_per_layer[kv.first];
        if (projs) m.expert_slice_bytes = std::max(m.expert_slice_bytes, kv.second / projs);
        m.token_cycle_bytes += kv.second * m.n_expert_used;
    }

    m.dense_bytes = m.file_bytes > m.expert_bytes ? m.file_bytes - m.expert_bytes : 0;
    m.is_moe = m.expert_bytes > 0 && m.n_expert > 0 && m.n_expert_used > 0;
    m.n_layer = (uint32_t) per_layer_one_expert.size(); // MoE layers, which are the ones that stream

    for (const auto & kv : projections_per_layer)
        projections_seen = std::max(projections_seen, kv.second);
    m.n_expert_projections = projections_seen;

    // llama.cpp builds the output head from the embedding table when the file carries no head of
    // its own, leaving two tensor objects over one range. Deciding it the same way the loader does
    // keeps the two from disagreeing.
    m.tied_output_head = saw_token_embd && !saw_output_weight;

    return m;
}

} // namespace bmoe
