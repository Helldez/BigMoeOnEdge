// Small dependency-free dot-product kernels for speculative-gating router prediction.
//
// The prediction scores every expert with dot(gate_row, hidden); on a 128-expert layer that is
// 128 matvecs over n_embd, run once per MoE layer per token on the prediction worker. These
// kernels give it a NEON path on arm64 (the deployment target) with a portable scalar fallback.
//
// Accumulation is in float, not double: the result only decides which experts are *prefetched*,
// never the model output (a mispredicted expert is a wasted read, corrected on demand), so the
// small precision difference versus a double accumulator is immaterial and never changes bytes.
#pragma once

#include <cstdint>

namespace bmoe {

// dot(w, h, n) for an f32 gate row and the f32 hidden state.
float spec_dot_f32(const float * w, const float * h, int n);

// dot(w, h, n) for an IEEE-half gate row (ggml_fp16_t is a uint16_t) and the f32 hidden state.
float spec_dot_f16(const uint16_t * w, const float * h, int n);

} // namespace bmoe
