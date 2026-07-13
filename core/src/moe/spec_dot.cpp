#include "spec_dot.h"

#include <cstring>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define BMOE_SPEC_NEON 1
#endif

namespace bmoe {

// Portable IEEE half → float, used by the scalar path and the NEON remainder. Router weights are
// normal numbers in practice, but the sub/inf/nan cases are handled for correctness at no cost.
static inline float half2float(uint16_t h) {
    const uint32_t sign = (uint32_t) (h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign; // +/- zero
        } else {
            int e = 0; // normalise the subnormal
            uint32_t m = man;
            while ((m & 0x400u) == 0) {
                m <<= 1;
                ++e;
            }
            m &= 0x3FFu;
            bits = sign | ((uint32_t) (127 - 15 - e) << 23) | (m << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (man << 13); // inf / nan
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

#if BMOE_SPEC_NEON

float spec_dot_f32(const float * w, const float * h, int n) {
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0, a2 = a0, a3 = a0;
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        a0 = vfmaq_f32(a0, vld1q_f32(w + i), vld1q_f32(h + i));
        a1 = vfmaq_f32(a1, vld1q_f32(w + i + 4), vld1q_f32(h + i + 4));
        a2 = vfmaq_f32(a2, vld1q_f32(w + i + 8), vld1q_f32(h + i + 8));
        a3 = vfmaq_f32(a3, vld1q_f32(w + i + 12), vld1q_f32(h + i + 12));
    }
    for (; i + 4 <= n; i += 4)
        a0 = vfmaq_f32(a0, vld1q_f32(w + i), vld1q_f32(h + i));
    float acc = vaddvq_f32(vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3)));
    for (; i < n; ++i)
        acc += w[i] * h[i];
    return acc;
}

float spec_dot_f16(const uint16_t * w, const float * h, int n) {
    // fp16→fp32 conversion (vcvt) is ARMv8-A baseline — no +fp16 arithmetic feature is needed.
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const float16x8_t wv = vreinterpretq_f16_u16(vld1q_u16(w + i));
        a0 = vfmaq_f32(a0, vcvt_f32_f16(vget_low_f16(wv)), vld1q_f32(h + i));
        a1 = vfmaq_f32(a1, vcvt_f32_f16(vget_high_f16(wv)), vld1q_f32(h + i + 4));
    }
    float acc = vaddvq_f32(vaddq_f32(a0, a1));
    for (; i < n; ++i)
        acc += half2float(w[i]) * h[i];
    return acc;
}

#else // scalar fallback (host builds: MSVC x64, etc.) — 4-way unrolled to help the vectorizer.

float spec_dot_f32(const float * w, const float * h, int n) {
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        a0 += w[i] * h[i];
        a1 += w[i + 1] * h[i + 1];
        a2 += w[i + 2] * h[i + 2];
        a3 += w[i + 3] * h[i + 3];
    }
    float acc = (a0 + a1) + (a2 + a3);
    for (; i < n; ++i)
        acc += w[i] * h[i];
    return acc;
}

float spec_dot_f16(const uint16_t * w, const float * h, int n) {
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        a0 += half2float(w[i]) * h[i];
        a1 += half2float(w[i + 1]) * h[i + 1];
        a2 += half2float(w[i + 2]) * h[i + 2];
        a3 += half2float(w[i + 3]) * h[i + 3];
    }
    float acc = (a0 + a1) + (a2 + a3);
    for (; i < n; ++i)
        acc += half2float(w[i]) * h[i];
    return acc;
}

#endif

} // namespace bmoe
