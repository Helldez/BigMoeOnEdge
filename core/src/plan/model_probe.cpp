// The model half of the planner's input: everything read from the gguf's own metadata and tensor
// shapes, with no tensor data touched and the model never loaded.
//
// Which tensors are experts comes from the architecture recipe — the one table in the engine that
// carries architecture knowledge — so a family the registry does not know produces a profile with
// no experts, and the planner declines rather than guessing at a layout. Every other name this
// file matches (`token_embd`, `output`, `blk.N.attn_*`, `blk.N.ffn_*`) is the gguf naming
// convention itself, shared by every architecture llama.cpp converts: a property of the FORMAT,
// not of any model. A name matching nothing lands in Other and is priced as read-whole, which is
// the conservative direction — an unrecognised tensor can make a plan cautious, never reckless.

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

// `blk.<il>.<rest>` -> (il, rest). Returns false for anything not inside a block, which is every
// top-level tensor. Does NOT require a `.weight` tail: classification and the MTP exclusion must
// see scales, biases and probs too, since those are bytes the file carries either way.
bool split_block(const std::string & name, int & layer, std::string & rest) {
    if (name.compare(0, 4, "blk.") != 0) return false;
    const size_t dot = name.find('.', 4);
    if (dot == std::string::npos) return false;
    const std::string num = name.substr(4, dot - 4);
    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) return false;
    layer = std::atoi(num.c_str());
    rest = name.substr(dot + 1);
    return true;
}

// As above, but yielding the suffix a recipe row is written against: the tensor name without the
// `.weight` the gguf appends. Returns false for scales, biases and probs, which are not the expert
// weight tensors however much their names resemble them.
bool split_expert_candidate(const std::string & name, int & layer, std::string & suffix) {
    if (!split_block(name, layer, suffix)) return false;
    const std::string tail = ".weight";
    if (suffix.size() <= tail.size() || suffix.compare(suffix.size() - tail.size(), tail.size(), tail) != 0)
        return false;
    suffix.resize(suffix.size() - tail.size());
    return true;
}

bool recipe_names(const MoeRecipe & r, const std::string & suffix) {
    for (int i = 0; i < MoeRecipe::max_exps; ++i)
        if (r.exps_suffix[i] && suffix == r.exps_suffix[i]) return true;
    return false;
}

// An embedding table, by the format's own naming. Two tensors qualify and both are gathered by
// row: `token_embd`, and the per-layer n-gram table some architectures add (gemma4, qwen4exp),
// which on one released quant is 28.8 GB — 43% of the file. Pricing that as read-whole would put
// a demand on every token that no token has, and it would dominate every other term in the plan.
// Matched by the convention rather than by name so a third table needs no new case.
bool is_embedding_table(const std::string & name) {
    return name.find("token_embd") != std::string::npos;
}

} // namespace

ModelProfile probe_model(const char * model_path) {
    ModelProfile m;
    if (!model_path || !*model_path) {
        m.error = "no model path";
        return m;
    }

    const GgufMeta meta = read_gguf_meta(model_path);
    if (!meta.ok) {
        m.error = std::string("cannot read gguf: ") + model_path;
        return m;
    }

    m.arch = meta.info.arch;
    m.n_expert = (uint32_t) std::max(0, meta.info.n_expert);
    m.n_expert_used = (uint32_t) std::max(0, meta.info.n_expert_used);

    const MoeRecipe * recipe = find_moe_recipe(m.arch.c_str());
    if (m.n_expert > 1 && !recipe) {
        // The same refusal the streamer makes: without a recipe the expert tensors cannot be
        // identified, and a plan that guessed at them would be worse than no plan.
        m.error = "no MoE recipe for architecture '" + m.arch + "': cannot identify expert tensors";
        return m;
    }
    m.ok = true;

    // Per MoE layer: the sum over its expert tensors of ONE expert's bytes. Accumulated from the
    // tensors the file actually carries rather than multiplied out, because the per-projection
    // stride is not uniform — a fused gate_up is twice a split one — and an architecture with
    // leading dense blocks has layers that contribute nothing.
    std::map<int, uint64_t> per_layer_one_expert;
    std::map<int, uint64_t> per_layer_expert_bytes;
    std::map<int, uint32_t> projections_per_layer;
    std::set<int> blocks;     // every block index the file carries, MTP excluded
    std::set<int> mtp_layers; // blocks that carry a multi-token-prediction head

    bool saw_output_weight = false;
    bool saw_token_embd = false;
    uint64_t embd_row_bytes = 0; // bytes one token gathers from the embedding tables
    bool embd_rows_known = true;

    // First pass: which blocks carry a multi-token-prediction head. llama.cpp does not load that
    // block at all unless asked, experts AND dense alike, so nothing of it may count toward what
    // the run holds. (Measured: counting its dense tensors overstated the pinned set by ~850 MiB.)
    for (const auto & kv : meta.offsets.size_by_name) {
        if (kv.first.find("nextn") == std::string::npos) continue;
        m.has_mtp = true;
        int layer = 0;
        std::string rest;
        if (split_block(kv.first, layer, rest)) mtp_layers.insert(layer);
    }

    for (const auto & kv : meta.offsets.size_by_name) {
        const std::string & name = kv.first;
        const uint64_t size = kv.second;

        int layer = 0;
        std::string rest;
        const bool in_block = split_block(name, layer, rest);
        if (in_block && mtp_layers.count(layer)) continue; // never loaded
        if (in_block) blocks.insert(layer);

        m.file_bytes += size;

        if (name == "output.weight") saw_output_weight = true;
        if (name == "token_embd.weight") saw_token_embd = true;

        int elayer = 0;
        std::string suffix;
        const bool expert_candidate = split_expert_candidate(name, elayer, suffix);
        const bool is_expert = recipe && m.n_expert > 0 && expert_candidate && recipe_names(*recipe, suffix);

        WeightGroup g = WeightGroup::Other;
        if (is_expert) {
            g = WeightGroup::Experts;
        } else if (!in_block && is_embedding_table(name)) {
            g = WeightGroup::Embedding;
            // A table whose row count the file does not state cannot be turned into a per-token
            // demand, and guessing one is how a plan claims a saving it cannot collect.
            const auto r = meta.offsets.rows_by_name.find(name);
            if (r != meta.offsets.rows_by_name.end() && r->second > 0)
                embd_row_bytes += size / r->second;
            else
                embd_rows_known = false;
        } else if (name == "output.weight") {
            g = WeightGroup::Output;
        } else if (in_block && rest.compare(0, 4, "attn") == 0) {
            g = WeightGroup::Attention;
        } else if (in_block && rest.compare(0, 3, "ffn") == 0) {
            g = WeightGroup::DenseFfn;
        }
        m.group(g).bytes += size;

        if (is_expert) {
            const uint64_t one = size / m.n_expert;
            per_layer_expert_bytes[elayer] += size;
            per_layer_one_expert[elayer] += one;
            projections_per_layer[elayer]++;
            if (m.expert_type_id < 0) {
                const auto t = meta.offsets.type_by_name.find(name);
                if (t != meta.offsets.type_by_name.end()) m.expert_type_id = t->second;
            }
        } else {
            m.largest_dense_tensor = std::max(m.largest_dense_tensor, size);
        }
    }

    for (const auto & kv : per_layer_expert_bytes)
        m.expert_bytes += kv.second;
    for (const auto & kv : per_layer_one_expert) {
        // The granule the streamer issues as a read is one expert of one projection; the LARGEST
        // of them is what should be looked up on the storage rate curve, because that is the
        // request the engine will actually make.
        const uint32_t projs = projections_per_layer[kv.first];
        if (projs) m.expert_slice_bytes = std::max(m.expert_slice_bytes, kv.second / projs);
        m.token_cycle_bytes += kv.second * m.n_expert_used;
    }

    m.dense_bytes = m.file_bytes > m.expert_bytes ? m.file_bytes - m.expert_bytes : 0;
    m.is_moe = m.expert_bytes > 0 && m.n_expert > 0 && m.n_expert_used > 0;
    m.n_layer = (uint32_t) blocks.size();                   // llama.cpp's own layer indices
    m.n_moe_layer = (uint32_t) per_layer_one_expert.size(); // the ones that stream

    uint32_t projections_seen = 0;
    for (const auto & kv : projections_per_layer)
        projections_seen = std::max(projections_seen, kv.second);
    m.n_expert_projections = projections_seen;

    // llama.cpp builds the output head from the embedding table when the file carries no head of
    // its own, leaving two tensor objects over one range. Deciding it the same way the loader does
    // keeps the two from disagreeing.
    m.tied_output_head = saw_token_embd && !saw_output_weight;

    // ── demand per token ────────────────────────────────────────────────────────────
    // Read whole, unless said otherwise below. Nothing here consults a size or an architecture.
    for (int i = 0; i < (int) WeightGroup::count; ++i)
        m.groups[i].bytes_per_token = m.groups[i].bytes;

    // The embedding tables are gathered by row — unless the head is tied to one of them, in which
    // case that table is projected against in FULL every token. The presence of a separate output
    // tensor decides it, not a size and not a name. When the head is tied the whole group is
    // priced read-whole, which overstates a per-layer table's demand and is the safe direction.
    GroupDemand & emb = m.group(WeightGroup::Embedding);
    if (emb.bytes > 0 && !m.tied_output_head && embd_rows_known && embd_row_bytes > 0) {
        emb.row_gatherable = true;
        emb.streamable = true;
        emb.bytes_per_token = embd_row_bytes;
    }

    // Experts are read whole but SELECTIVELY: only the routed top-k of each layer. This is the
    // same quantity as the cache floor, and it is deliberately the same number.
    GroupDemand & exp = m.group(WeightGroup::Experts);
    if (exp.bytes > 0 && m.is_moe) {
        exp.bytes_per_token = m.token_cycle_bytes;
        exp.streamable = true;
    }

    return m;
}

} // namespace bmoe
