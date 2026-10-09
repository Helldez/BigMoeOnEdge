// What this gguf is, and what it demands per token, as facts a planning rule may read.
//
// The companion to HardwareProfile: between them they are the planner's whole input. Everything
// here is discovered from the file's own metadata and tensor shapes — no architecture name decides
// anything, no size threshold is written down, and a model family the engine has never seen
// produces a profile like any other. Where a field cannot be discovered it stays 0, and the rules
// that need it decline.
//
// The model asks for wildly different things from different parts of itself, which is the entire
// reason a single global policy leaves throughput on the table: a 30B MoE reads every byte of its
// attention weights per token and roughly one row of its embedding table, and pricing both as
// "dense weights" prices things that differ by three orders of magnitude identically. So the file
// is decomposed into groups by ACCESS SHAPE, and each carries its own per-token demand.
//
// On not hardcoding: the tensor names that identify EXPERT weights come from the architecture
// recipe (bmoe/recipe.h), so a new MoE family stays one table row. The remaining names —
// `token_embd`, `output`, `blk.N.attn_*`, `blk.N.ffn_*` — are the gguf naming convention itself,
// which every architecture llama.cpp converts shares; they are a property of the FORMAT, not of
// any model, and a name that matches nothing lands in Other and is priced as read-whole, which is
// the conservative direction: an unrecognised tensor can make a plan cautious, never reckless.
//
// Pure policy: no llama.cpp, no I/O. See core/src/plan/model_probe.cpp for the adapter that fills
// this from a gguf, and bmoe/planner.h for what reads it.
#pragma once

#include <cstdint>
#include <string>

namespace bmoe {

// Weight groups that want different treatment, ordered roughly by how much a plan can win on
// them. The split is by ACCESS SHAPE, not by role: two tensors belong together when the same
// residency decision is right for both.
enum class WeightGroup {
    Embedding, // gathered by row, one row per token — resident for almost nothing
    Attention, // read whole, every token, every layer
    DenseFfn,  // read whole, every token: the shared (non-expert) FFN path
    Experts,   // read whole but SELECTIVELY: only the routed top-k of each layer
    Output,    // read whole, once per token, and only if it is a distinct tensor
    Other,     // norms, biases, anything unrecognised: small, and priced as read-whole
    count,
};

const char * group_name(WeightGroup g);

struct GroupDemand {
    uint64_t bytes = 0; // total size of the group in the file, MTP blocks excluded

    // Bytes one decoded token must obtain, from wherever they live. For Experts this is already
    // the top-k share, not the whole expert set: it is the demand, not the footprint.
    uint64_t bytes_per_token = 0;

    // The graph only ever gathers ROWS from this group, so a resident copy buys residency for
    // bytes that will never be read. Only the embedding table can qualify, and only when it is
    // not also serving as the output head.
    bool row_gatherable = false;

    // The engine has a lane that can serve this group from flash today. A group that is not
    // streamable is a residency decision only; the planner may not propose a lane that does not
    // exist, and saying so here is how that stays true as lanes are added.
    bool streamable = false;
};

struct ModelProfile {
    std::string arch; // for the rationale and for the record; never branched on
    bool is_moe = false;

    // Two different counts, and conflating them is a bug this header exists to prevent. The
    // absolute block index is llama.cpp's coordinate system — it is what `n_gpu_layers` counts
    // down from and what an override pattern names — while only some of those blocks carry
    // experts. An architecture with leading dense blocks (lfm2moe, bailingmoe3) has fewer of the
    // second than the first, so a pro-rata or a placement computed against the wrong one is
    // silently wrong on exactly the architectures that motivated the distinction.
    uint32_t n_layer = 0;     // blocks in the file: llama.cpp's own layer indices
    uint32_t n_moe_layer = 0; // of those, the ones carrying expert tensors — the ones that stream

    uint32_t n_expert = 0;      // experts per MoE layer
    uint32_t n_expert_used = 0; // routed per token (top-k)

    // The hidden state's width. Not a size the streamer cares about — it is what a tensor handed
    // across a host/device boundary actually carries, so it is the shape the split probe measures
    // a crossing at. Zero when the file does not state it, and the probe then declines.
    uint32_t n_embd = 0;

    // How many expert tensors a layer has: three where gate/up/down are separate, two where gate
    // and up are fused. Read from which tensors the file actually carries, so a new fusion is a
    // different count rather than a new case.
    uint32_t n_expert_projections = 0;

    // ── bytes ───────────────────────────────────────────────────────────────────────
    // Everything below EXCLUDES the multi-token-prediction block. llama.cpp marks its tensors
    // TENSOR_SKIP and does not load them at all unless --mtp asks, experts and dense alike, so
    // counting them overstates what the run holds — measured at ~850 MiB on one model.
    uint64_t file_bytes = 0;           // the whole model, every shard
    uint64_t expert_bytes = 0;         // every routed-expert tensor
    uint64_t dense_bytes = 0;          // everything else: embeddings, attention, norms, head
    uint64_t largest_dense_tensor = 0; // the one a residency policy may not be able to hold

    // Every expert tensor of the MoE layer that has the most of them, in bytes. It is the unit a
    // device prefill reserves in: the layer being computed and the one being loaded behind it each
    // need room for a whole layer's experts, so two of these is what arming one costs.
    uint64_t largest_expert_layer_bytes = 0;
    // The same for a block's NON-expert tensors (attention, norms, shared experts): under
    // streaming a prefill device carries those through two slots of their own.
    uint64_t largest_layer_dense_bytes = 0;

    // The same bytes again, decomposed by access shape. `groups[...].bytes` sums to `file_bytes`,
    // and Experts::bytes equals `expert_bytes`; both spellings are kept because a capacity check
    // wants the totals and the cost model wants the per-group demand.
    GroupDemand groups[(int) WeightGroup::count];

    // One expert's slice of the largest projection in a layer: the granule the streamer actually
    // issues as a read, and therefore the request size to look up on the storage rate curve. This
    // is the bridge between the model and the hardware — the same granule sits at a different point
    // of a different machine's curve, which is why no lane count or request size can be a constant.
    //
    // The LARGEST rather than an average over every expert tensor: the curve is looked up to price
    // the read the engine will actually issue, and on a fused gate_up layout the two projections
    // differ by 2x, so an average prices a request nobody makes.
    uint64_t expert_slice_bytes = 0;

    // Bytes one token can demand at worst: every MoE layer routing a disjoint set. This is the
    // floor any expert cache must clear — under it the cache evicts what the same token still
    // needs, so it costs its memory and returns no hits at all.
    //
    // Summed over the tensors the file actually carries rather than multiplied out, because the
    // factors are not uniform: a fused gate_up projection is twice the stride of a split one, and
    // an architecture with leading dense blocks has layers that demand nothing. A product would be
    // wrong on both, and wrong in the direction that under-sizes the floor.
    uint64_t token_cycle_bytes = 0;

    // The quantization the expert tensors carry, as a ggml type id, or -1 when unknown. Kept as an
    // opaque integer so this header stays free of ggml; it exists to be handed back to a backend
    // when asking whether that backend can execute this model's expert matmul natively — and to
    // make a bandwidth probe measure the material the run is actually made of. Measuring in F32
    // instead answered 2 threads on a phone where the engine is 58% faster at 4: an F32 GEMV
    // saturates the bus at very few threads, a quantized one carries dequantization work per byte
    // and keeps scaling.
    int expert_type_id = -1;

    // ── shapes that constrain what may be done ──────────────────────────────────────
    // The output head is the embedding table. Two tensor objects over one range, so a policy that
    // rebinds one and not the other leaves the graph reading the mapping every token — and the
    // table is then projected against in full every token, which disqualifies it from row
    // streaming however large it is. No size threshold or architecture name is involved.
    bool tied_output_head = false;
    bool has_mtp = false; // a trained multi-token-prediction block is present

    bool ok = false; // false when the file could not be read; every rule declines
    std::string error;

    const GroupDemand & group(WeightGroup g) const { return groups[(int) g]; }
    GroupDemand & group(WeightGroup g) { return groups[(int) g]; }

    // Bytes one token's routing reads when nothing is cached: the MECHANICAL floor of the expert
    // cache. The same quantity as `token_cycle_bytes`, named for the question it answers.
    uint64_t expert_working_set_bytes() const { return token_cycle_bytes; }

    // Bytes a token must obtain if NOTHING is resident: the ceiling the plan works down from.
    uint64_t bytes_per_token() const;
};

} // namespace bmoe
