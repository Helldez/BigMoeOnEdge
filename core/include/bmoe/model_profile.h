// What this gguf is, as facts a planning rule may read.
//
// The companion to HardwareProfile: between them they are the planner's whole input. Everything
// here is discovered from the file's own metadata and tensor shapes — no architecture name decides
// anything, no size threshold is written down, and a model family the engine has never seen
// produces a profile like any other. Where a field cannot be discovered it stays 0, and the rules
// that need it decline.
//
// Pure policy: no llama.cpp, no I/O. See core/src/plan/model_probe.cpp for the adapter that fills
// this from a gguf, and bmoe/planner.h for what reads it.
#pragma once

#include <cstdint>
#include <string>

namespace bmoe {

struct ModelProfile {
    std::string arch; // for the rationale and for the record; never branched on
    bool is_moe = false;

    uint32_t n_layer = 0;
    uint32_t n_expert = 0;      // experts per MoE layer
    uint32_t n_expert_used = 0; // routed per token (top-k)

    // How many expert tensors a layer has: three where gate/up/down are separate, two where gate
    // and up are fused. Read from which tensors the file actually carries, so a new fusion is a
    // different count rather than a new case.
    uint32_t n_expert_projections = 0;

    // ── bytes ───────────────────────────────────────────────────────────────────────
    uint64_t file_bytes = 0;           // the whole model, every shard
    uint64_t expert_bytes = 0;         // every routed-expert tensor
    uint64_t dense_bytes = 0;          // everything else: embeddings, attention, norms, head
    uint64_t largest_dense_tensor = 0; // the one a residency policy may not be able to hold

    // One expert's slice of the largest projection in a layer: the granule the streamer actually
    // issues as a read, and therefore the request size to look up on the storage rate curve. This
    // is the bridge between the model and the hardware — the same granule sits at a different point
    // of a different machine's curve, which is why no lane count or request size can be a constant.
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

    // ── shapes that constrain what may be done ──────────────────────────────────────
    // The output head is the embedding table. Two tensor objects over one range, so a policy that
    // rebinds one and not the other leaves the graph reading the mapping every token.
    bool tied_output_head = false;
    bool has_mtp = false; // a trained multi-token-prediction block is present

    bool ok = false; // false when the file could not be read; every rule declines
};

} // namespace bmoe
