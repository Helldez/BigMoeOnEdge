// The machine half of the planner's input. See bmoe/probe.h for the boundary this file is on
// the wrong side of, deliberately: platform conditionals belong here and nowhere above.

#include "bmoe/probe.h"

#include "../io/platform_io.h"

#include "ggml-backend.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace bmoe {

namespace {

// What a reclaim costs here. The question the dense policy actually asks is not "which OS is this"
// but "if the kernel wants this memory back, what happens to us", and the honest answer comes from
// what the machine has to reclaim INTO — which is why this looks at the swap devices rather than at
// a platform name. A desktop Linux with zram configured is Compress, and gets the phone's policy
// for the phone's reason.
Overflow probe_anon_overflow() {
#if defined(_WIN32)
    // A pagefile is present on any default install; anonymous pages are written to it and the
    // process survives.
    return Overflow::Swap;
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
    // No user swap: memory pressure is answered by terminating the process, and the limit is the
    // process's own rather than the system's.
    return Overflow::Kill;
#else
    return Overflow::Compress; // the VM compressor, not a swap device
#endif
#else
    // /proc/swaps lists the backing devices. A zram device compresses in RAM (cheap to lose, and
    // invisible to every I/O counter); a partition or file writes out (slow, survivable); nothing
    // at all means anonymous memory cannot be evicted, so pressure ends in a kill.
    FILE * f = std::fopen("/proc/swaps", "re");
    if (!f) return Overflow::Unknown;
    char line[512];
    bool any = false, zram = false;
    bool first = true;
    while (std::fgets(line, sizeof(line), f)) {
        if (first) { // header row
            first = false;
            continue;
        }
        if (line[0] == '\n' || line[0] == '\0') continue;
        any = true;
        if (std::strstr(line, "zram")) zram = true;
    }
    std::fclose(f);
    if (zram) return Overflow::Compress;
    if (any) return Overflow::Swap;
    return Overflow::Kill;
#endif
}

// Whether clean mapped file pages sit outside the limit that can end this run. Only asked where
// exceeding is fatal; everywhere else the dense rule never reaches it, so leaving it Unknown costs
// nothing and claims nothing.
Tri probe_file_pages_counted() {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // The per-process limit is on dirty memory; clean file-backed pages can be evicted and are not
    // charged, which is what makes a mapped weight the cheap residency here.
    return Tri::No;
#else
    return Tri::Unknown;
#endif
}

void probe_devices(HardwareProfile & h) {
    // Nothing is registered before llama_backend_init(); an empty list here simply means the caller
    // probed early, and every rule that reads devices treats it as "no device-local memory".
    const size_t n_reg = ggml_backend_reg_count();
    for (size_t i = 0; i < n_reg; ++i) {
        ggml_backend_reg_t reg = ggml_backend_reg_get(i);
        if (!reg) continue;
        const size_t n_dev = ggml_backend_reg_dev_count(reg);
        for (size_t j = 0; j < n_dev; ++j) {
            ggml_backend_dev_t dev = ggml_backend_reg_dev_get(reg, j);
            if (!dev) continue;
            ggml_backend_dev_props props{};
            ggml_backend_dev_get_props(dev, &props);

            ComputeDevice d;
            d.name = props.name ? props.name : "";
            d.description = props.description ? props.description : "";
            d.memory_free = props.memory_free;
            d.memory_total = props.memory_total;
            // An integrated device's memory IS host memory: moving a tensor off it frees nothing,
            // which is the fact that makes the capacity tier nearly inert on such a machine.
            d.host_memory = props.type == GGML_BACKEND_DEVICE_TYPE_CPU || props.type == GGML_BACKEND_DEVICE_TYPE_IGPU;
            // Only a host buffer can have its tensor `data` repointed at bytes we read ourselves,
            // which is the condition the expert streamer exists under. A device-local buffer's
            // pointer is not a host address.
            d.rebindable = d.host_memory;
            h.devices.push_back(std::move(d));
        }
    }
}

} // namespace

HardwareProfile probe_hardware(const char * model_path) {
    HardwareProfile h;

    h.residency_budget = pio::mem_available_bytes();
    h.anon_overflow = probe_anon_overflow();
    h.reclaim_exempt_max = pio::pinned_max_bytes();
    h.file_pages_counted = probe_file_pages_counted();
    h.n_cores = std::thread::hardware_concurrency();

    probe_devices(h);

    if (model_path && *model_path) {
        // Whether uncached reads are actually in effect is a property of this path, not of the
        // platform: the same device answers yes on one filesystem and declines on another. Ask the
        // open what it got rather than reconstructing it from what was requested.
        bool effective = false;
        pio::fd_t fd = pio::open_read(model_path, /*direct=*/true, &effective);
        if (pio::fd_ok(fd)) {
            h.storage.direct_ok = effective ? Tri::Yes : Tri::No;
            pio::close_fd(fd);
        }
    }

    // Left Unknown on purpose: both cost real I/O to answer, and a plan that says "unprobed" is
    // worth more than one that assumes. See bmoe/probe.h.
    h.storage.mapping_serialises_reads = Tri::Unknown;

    h.label = std::to_string(h.n_cores) + " cores, " + std::to_string((unsigned long long) (h.residency_budget >> 20)) +
              " MiB available";
    return h;
}

} // namespace bmoe
