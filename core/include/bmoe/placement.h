// The first stage: what llama.cpp's own capacity fitter decided for this model on this machine.
//
// llama.cpp ships a fitter (`common_fit_params`) that loads the model with no_alloc, measures the
// memory it would take per device, and if it does not fit first shrinks the context and then moves
// weights from device memory into system memory — per class inside each layer, attention first
// and the sparse expert tensors last. It sees every backend through ggml and knows nothing about
// flash: the comment above it reads "assumes system memory is unlimited". That line is where the
// second stage, the expert streamer, begins.
//
// This header is what the second stage reads from the first: which layers keep their experts on
// the host, and how much host memory the placed model will take. It is pure policy — no llama.cpp
// types — so the planner can be tested against a synthetic placement.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bmoe {

struct Placement {
    // Whether the fitter ran and produced this. False means the caller planned without it, and
    // every field below is what the engine does on its own: no layers on any device.
    bool fitted = false;
    std::string outcome; // the fitter's own one-line verdict, for the rationale

    int32_t n_gpu_layers = 0;
    uint32_t n_ctx = 0; // the context the fitter settled on (it shrinks this first)

    // Layers whose routed experts the fitter left on the host. These are the layers the streamer
    // can serve; experts the fitter placed on a device are not ours and are never read from flash.
    std::vector<uint32_t> host_expert_layers;

    // Host memory the placed model is projected to occupy, excluding the expert tensors of
    // `host_expert_layers` (which the streamer takes off the residency books) — the dense set that
    // stayed on the host, plus KV and compute buffers that live in host memory. This is what the
    // second stage subtracts from the residency budget before sizing its cache.
    uint64_t host_resident_bytes = 0;

    // Device-local memory the placement uses, summed over devices with memory of their own.
    uint64_t device_bytes = 0;

    // True when the fitter had a device to place on, but one whose memory is the host's - so its
    // capacity arithmetic counted the same pool twice and its placement was NOT applied. Carried so
    // the plan can say that out loud instead of reporting "no device" on a machine that has one.
    bool shared_memory_placement = false;

    // The fitter's own host breakdown, raw, so the derived figures above can be checked against
    // it: what it projects for the model's host buffers, the context (KV), and the compute buffers.
    uint64_t raw_host_model_bytes = 0;
    uint64_t raw_host_context_bytes = 0;
    uint64_t raw_host_compute_bytes = 0;

    // The compute buffers the fitter projects on the devices, summed. Kept apart from
    // `device_bytes` because it is the part a device costs for being COMPUTED on, whatever weights
    // are or are not placed there - which is what a device prefill pays.
    uint64_t device_compute_bytes = 0;
    uint64_t device_context_bytes = 0; // the context the fitter put on the devices, summed

    // The override patterns the fitter wrote, kept verbatim so the session can reapply them at load
    // (they are what routes the placed experts to a device buffer type).
    std::vector<std::string> override_patterns;
};

} // namespace bmoe
