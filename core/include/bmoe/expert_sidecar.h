// Contiguous per-expert sidecar file.
//
// Motivation (measured, 2026-07-20, bmoe-iobench --scatter on device): the flash serves
// scattered per-projection expert slices (~0.3 MB, today's gguf layout) at a bandwidth
// plateau ~10% below its ceiling even at 16 lanes, while the same bytes laid out as one
// contiguous window per expert reach the ceiling at 2-4 lanes — and the engine's real
// operating point is ~2 effective lanes, where the gap is ~35%. The sidecar re-orders the
// expert bytes on disk so one routed expert costs ONE read instead of one per projection.
//
// It is a pure byte re-ordering of the gguf's expert tensor data — same weights, same
// quantization, zero quality change — in a SEPARATE file the engine's own read path
// consumes. The gguf stays untouched and remains the single source of truth: llama.cpp
// still loads it (metadata + dense weights), and the sidecar carries the identity of the
// gguf it was built from (file size + a hash of the expert offset map), so a stale or
// mismatched sidecar is detected and refused at init rather than silently corrupting.
//
// Layout: header + per-layer table, then for each bound layer (ascending), for each
// expert e in [0, n_expert): the expert's present projection slices concatenated in
// recipe slot order, each entry starting on an `align` boundary (O_DIRECT windows).
//
// No architecture constants live here: the expert map (which tensors, strides, offsets)
// is discovered from the gguf through the same recipe registry the streamer uses.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bmoe {

// One bound layer's row in the sidecar table, as stored on disk.
struct SidecarLayer {
    uint32_t il = 0;                    // model layer index
    uint64_t base_off = 0;              // file offset of expert 0's entry
    uint64_t entry_stride = 0;          // aligned bytes between consecutive experts' entries
    uint64_t proj_bytes[3] = {0, 0, 0}; // slice bytes per recipe slot; 0 = absent slot
};

// Parsed and validated sidecar header. `ok` is false on any structural problem; identity
// against a live gguf (size + hash) is the caller's check — see expert_map_hash below.
struct SidecarIndex {
    bool ok = false;
    std::string error;
    uint32_t align = 0;
    uint64_t source_size = 0; // gguf byte size the sidecar was built from
    uint64_t source_hash = 0; // expert_map_hash of that gguf's expert map
    uint32_t n_expert = 0;
    std::vector<SidecarLayer> layers;
};

// The identity both sides must agree on: FNV-1a over (n_expert, then per bound layer
// ascending: il, then per projection slot: absolute file offset and slice bytes, 0/0 for
// absent slots). The builder computes it from the gguf tensor table; the streamer
// recomputes it from the layers it actually bound. Equal hash + equal file size means the
// sidecar's entries describe exactly the experts the engine is about to stream.
uint64_t expert_map_hash(uint32_t n_expert, const std::vector<SidecarLayer> & layers);

// Derive the canonical sidecar path for a model: "<gguf_path>.experts.bmoe".
std::string sidecar_path_for(const std::string & gguf_path);

// Build the sidecar next to the gguf (or at an explicit path). Discovers the expert map
// via the gguf metadata + the recipe registry, streams the bytes across, fsyncs, and
// writes the magic LAST so a torn build never validates. `progress` (optional) is called
// with (bytes_done, bytes_total) at a coarse cadence. Returns false with `error` set on
// any failure. One-shot offline work: buffered I/O, no engine state.
bool build_expert_sidecar(const std::string & gguf_path,
                          const std::string & out_path,
                          std::string & error,
                          const std::function<void(uint64_t, uint64_t)> & progress = nullptr);

// Load and structurally validate a sidecar's header + table. Identity vs the live model
// is NOT checked here (the loader has no gguf) — callers compare source_size/source_hash.
SidecarIndex load_sidecar_index(const std::string & path);

} // namespace bmoe
