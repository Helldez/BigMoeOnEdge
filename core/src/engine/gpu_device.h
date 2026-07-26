// GPU device discovery.
//
// The engine never names a vendor or a graphics API. It asks the ggml backend registry what
// devices this build registered and takes the first one that is a GPU, which keeps the policy
// ("offload the dense path if there is somewhere to offload it to") independent of whether the
// backend underneath is OpenCL on an Adreno, Vulkan on a Mali, or something that does not exist
// yet. A build compiled without any GPU backend, and a device whose driver is missing or refuses
// to initialise, both land in the same place: no device found, and the caller decides whether
// that is a fallback or an error.
#pragma once

#include "ggml-backend.h"

#include <cstdint>
#include <string>

namespace bmoe {

// A GPU the registry offered, or `found == false`. The handle is owned by the registry and stays
// valid for the process lifetime; it is passed straight to llama_model_params::devices.
struct GpuDevice {
    bool found = false;
    ggml_backend_dev_t dev = nullptr;
    std::string name;        // registry device name, e.g. "GPUOpenCL"
    std::string description; // driver-reported description, e.g. "QUALCOMM Adreno(TM) 830"
    std::string backend;     // registry (backend) name, e.g. "OpenCL"
    uint64_t mem_free = 0;   // bytes the driver reports free; 0 when it does not report
    uint64_t mem_total = 0;  // bytes the driver reports total; 0 when it does not report
};

// Find a usable GPU. Discrete GPUs are preferred over integrated ones only because that is the
// registry's own ordering convention; on the devices this engine targets the two are the same
// silicon. Safe to call before llama_backend_init() — it loads any dynamic backends itself.
GpuDevice find_gpu_device();

// One line describing what was found, for the run banner and the telemetry preamble. Returns
// "none" when nothing was found, so the caller can log unconditionally.
std::string describe(const GpuDevice & g);

} // namespace bmoe
