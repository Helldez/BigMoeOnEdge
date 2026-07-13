// Read every tensor's absolute byte offset in a gguf file, using only the public gguf
// API. The expert streamer needs these offsets to pread individual expert slices; it
// gets the tensor pointers (to rebind) from the graph, and the file layout from here.
// The two are matched by tensor name.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace bmoe {

struct GgufOffsets {
    // tensor name -> absolute file offset of its data (data_offset + per-tensor offset)
    std::unordered_map<std::string, uint64_t> off_by_name;
    bool ok = false;
};

// Parse `path` with no_alloc (metadata only, no tensor data read into RAM) and collect
// the offset of every tensor. Returns ok=false if the file cannot be opened as gguf.
GgufOffsets read_gguf_offsets(const char * path);

// The handful of model metadata needed BEFORE the model is loaded: the architecture (to
// build arch-prefixed metadata keys) and the MoE expert counts. Read via the public gguf
// API, so no per-architecture constants leak into the engine — the arch string drives the
// key names. n_expert/n_expert_used are 0 for a non-MoE model or a missing key.
struct GgufModelInfo {
    std::string arch;
    int n_expert = 0;
    int n_expert_used = 0;
    bool ok = false;
};

// Peek `path`'s metadata (no_alloc, no tensor bytes) for the fields above. Returns ok=false
// if the file cannot be opened as gguf; a present file with a missing key leaves that field 0.
GgufModelInfo read_gguf_model_info(const char * path);

} // namespace bmoe
