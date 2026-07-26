#include "gpu_device.h"

#include <cstdio>

namespace bmoe {

namespace {

// Fill the descriptive fields once a device has been chosen. Split out so the two acceptance
// passes below stay about *selection* and cannot disagree about what they report.
GpuDevice describe_device(ggml_backend_dev_t dev) {
    GpuDevice g;
    g.found = true;
    g.dev = dev;

    const char * name = ggml_backend_dev_name(dev);
    const char * desc = ggml_backend_dev_description(dev);
    g.name = name ? name : "";
    g.description = desc ? desc : "";

    if (ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(dev)) {
        const char * rn = ggml_backend_reg_name(reg);
        g.backend = rn ? rn : "";
    }

    // Not every driver answers this; ggml zeroes what it cannot fill, and 0 reads as "unknown"
    // rather than "empty" at every call site.
    size_t free_b = 0, total_b = 0;
    ggml_backend_dev_memory(dev, &free_b, &total_b);
    g.mem_free = (uint64_t) free_b;
    g.mem_total = (uint64_t) total_b;
    return g;
}

} // namespace

GpuDevice find_gpu_device() {
    // llama_backend_init() also does this, but discovery has to work for a caller that wants to
    // know what is available *before* committing to a load. The registry counts as loaded once it
    // holds anything, so the repeat call is free.
    if (ggml_backend_reg_count() == 0) {
        ggml_backend_load_all();
    }

    // Two passes rather than one with a preference score: a discrete GPU is unambiguously the
    // better target when both are present, and the integrated fallback is what phones actually
    // offer. Anything else the registry exposes (CPU, BLAS/AMX accelerators, the tensor-parallel
    // meta device) is not something the dense path can be handed to.
    for (enum ggml_backend_dev_type want : {GGML_BACKEND_DEVICE_TYPE_GPU, GGML_BACKEND_DEVICE_TYPE_IGPU}) {
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            ggml_backend_dev_t dev = ggml_backend_dev_get(i);
            if (dev && ggml_backend_dev_type(dev) == want) {
                return describe_device(dev);
            }
        }
    }
    return {};
}

std::string describe(const GpuDevice & g) {
    if (!g.found) {
        return "none";
    }
    std::string s = g.name;
    if (!g.backend.empty()) {
        s += " (" + g.backend + ")";
    }
    if (!g.description.empty() && g.description != g.name) {
        s += " — " + g.description;
    }
    if (g.mem_total > 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), ", %llu/%llu MiB free", (unsigned long long) (g.mem_free >> 20),
                      (unsigned long long) (g.mem_total >> 20));
        s += buf;
    }
    return s;
}

} // namespace bmoe
