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

} // namespace bmoe
