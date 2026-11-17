#pragma once

// Pure cache-sizing arithmetic shared by the streamer and its tests. No llama.cpp, no platform.
//
// The decode-derived floor (MoeStreamConfig::cache_min_mb) prices one token's cycle. A wide CPU
// prefill is different: one ubatch routes to the union of its tokens, approaching every expert of a
// layer, so the cache must be able to hold one whole layer's expert set or it re-reads that layer's
// slices while the layer's matmul is still walking them. This is the missing floor the prefill
// budget needs; the answer is a size in bytes, not a policy decision.

#include <cstddef>
#include <limits>

namespace bmoe {

// Largest single layer's full expert set, capped at `cap`. `per_layer_bytes[i]` is one expert's
// bytes in layer i (every projection summed); `n_expert` is that layer's declared expert count.
// Multiplication is saturating so a malformed gguf cannot wrap the floor into zero.
inline std::size_t prefill_layer_floor_bytes(const std::size_t * per_layer_bytes, int n_layer,
                                             std::size_t n_expert, std::size_t cap) {
    if (n_layer <= 0 || n_expert == 0) return 0;
    std::size_t worst = 0;
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    for (int i = 0; i < n_layer; ++i) {
        const std::size_t b = per_layer_bytes[i];
        if (b == 0) continue;
        const std::size_t layer = b > max / n_expert ? max : b * n_expert;
        if (layer > worst) worst = layer;
    }
    return worst < cap ? worst : cap;
}

} // namespace bmoe
