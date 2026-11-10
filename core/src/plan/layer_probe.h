// The compute probes' shared pieces: what counts as two backends agreeing, and the real-layer probe
// the bandwidth probe hands the model's own layer to. Internal to core/src/plan.
#pragma once

#include "bmoe/hardware_profile.h"
#include "bmoe/model_profile.h"

#include <cstdint>
#include <vector>

namespace bmoe::detail {

// How many tokens the wide instrument is: several times the narrowest graph a prefill device is
// ever handed, so the sample sits inside the regime it speaks for. Shared by the real layer and the
// synthetic fallback, which report it as HardwareProfile::wide_batch.
constexpr int64_t k_wide_batch = 128;

// How long one sample must last, and the cap that keeps a slow device from turning a probe into a
// benchmark. Sized by DURATION rather than by a repetition count: a fixed count makes the sample
// shorter as the machine gets faster, and a two-millisecond sample on a general-purpose OS reports
// scheduling noise.
constexpr double k_sample_seconds = 0.15;
constexpr int k_reps_max = 512;

// Agreement, not bit-equality: llama.cpp's own normalized mean squared error, sum((a-b)^2) /
// sum(a^2), against the 5e-4 tests/test-backend-ops.cpp allows a MUL_MAT. See bandwidth_probe.cpp
// for the measurements behind taking upstream's bound rather than a stricter private one.
constexpr double k_max_nmse = 5e-4;
// A device judged against the exact result rather than against the host (probe_real_layer): twice
// upstream's per-op bound. Upstream's 5e-4 is a backend against the CPU on random data, and it relaxes
// it where a backend quantizes the activations its own way (2e-2 for MXFP4 activations); a wrong kernel
// is off by 1e-1 to 1. Measured on RTX 3090 cards: GLM-4.5-Air's Q5_0/Q5_1 experts at 5.3e-4 from the
// exact result, the host at 1.1e-5, every other layout measured at 6e-6 or less. The host must still be
// within k_max_nmse of the exact result to be the reference.
constexpr double k_max_nmse_device = 1e-3;
bool agrees(const std::vector<float> & a, const std::vector<float> & b);

// One real layer of the model, built from the file's own tensors and bytes, run on the host (at
// hw.best_threads) and on every device probed here, at decode width and at prefill width: the
// expert block and the dense matrices apart. Fills ComputeDevice::expert_gibs / dense_gibs /
// wide_matmul_gibs / wide_identity_ok, the host's counterparts, and hw.wide_batch, and refuses a
// device whose result disagrees with the host's at decode width (identity_ok = No). Returns false,
// touching nothing, where no layer could be read (no file, not a MoE, an unreadable tensor): the
// caller then keeps the synthetic matmul for what it filled before.
bool probe_real_layer(HardwareProfile & hw, const ModelProfile & model);

} // namespace bmoe::detail
