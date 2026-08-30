// The machine half of the planner's input. See bmoe/probe.h for the boundary this file is on
// the wrong side of, deliberately: platform conditionals belong here and nowhere above.

#include "bmoe/probe.h"

#include "../io/platform_io.h"

#include "ggml-backend.h"
#include "ggml.h"

#include <cstdio>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

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
    // What the machine reclaims INTO. /proc/swaps would say directly, but an unprivileged process
    // on Android may not read it (nor /sys/block/zram0's contents). Two things it CAN see suffice:
    // whether a zram block device EXISTS (the directory entry is visible even where its files are
    // not) and whether /proc/meminfo reports any swap at all. A zram device means anonymous memory
    // is compressed in RAM; swap without zram means it is written out; no swap at all means it
    // cannot be evicted, and pressure ends in a kill.
    struct stat st {};
    const bool has_zram = ::stat("/sys/block/zram0", &st) == 0;
    uint64_t swap_total_kb = 0;
    if (FILE * f = std::fopen("/proc/meminfo", "re")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long long v = 0;
            if (std::sscanf(line, "SwapTotal: %llu kB", &v) == 1) {
                swap_total_kb = v;
                break;
            }
        }
        std::fclose(f);
    } else {
        return Overflow::Unknown;
    }
    if (has_zram && swap_total_kb > 0) return Overflow::Compress;
    if (swap_total_kb > 0) return Overflow::Swap;
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

// The core classes this machine has, fastest first, as counts. A barrier waits for its slowest
// participant, so a thread rule that reads a core COUNT on a heterogeneous processor is reading the
// wrong number: on one twenty-core machine the tenth thread is the last fast core and the eleventh
// costs throughput. Where the classes cannot be read the vector stays empty, which every rule reads
// as "this machine did not say" rather than as "every core is alike".
void probe_core_classes(HardwareProfile & h) {
#if defined(__linux__)
    // Group the CPUs by the maximum frequency the kernel reports for each. It is a fact about this
    // machine, published per CPU, and it needs no table of processor names to interpret.
    std::vector<uint64_t> khz;
    for (uint32_t cpu = 0; cpu < h.n_cores; ++cpu) {
        char path[128];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", cpu);
        FILE * f = std::fopen(path, "re");
        if (!f) return; // partial information about a heterogeneous machine is worse than none
        unsigned long long v = 0;
        const int n = std::fscanf(f, "%llu", &v);
        std::fclose(f);
        if (n != 1 || v == 0) return;
        khz.push_back((uint64_t) v);
    }
    if (khz.empty()) return;
    std::sort(khz.begin(), khz.end(), std::greater<uint64_t>());
    for (size_t i = 0; i < khz.size();) {
        size_t j = i;
        while (j < khz.size() && khz[j] == khz[i])
            ++j;
        h.core_classes.push_back((uint32_t) (j - i));
        i = j;
    }
#else
    // Nothing here reports classes for free. A machine whose cores are alike is still describable,
    // but claiming that without evidence is exactly the guess this design refuses to make.
    (void) h;
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
            // Whether this device will execute over memory the host owns. Asking the backend for a
            // host buffer type is the question; a device that answers is one we can read flash into
            // and it can compute out of, which is exactly the pair of properties a streamed expert
            // needs. Host memory has it by definition, and a device with memory of its own may still
            // offer it - which is the difference between an accelerator that can serve streamed
            // experts and one that can only be handed a copy it repacks.
            d.host_buffer = d.host_memory || ggml_backend_dev_host_buffer_type(dev) != nullptr ? Tri::Yes : Tri::No;
            // Only a host address can be repointed at bytes we read ourselves, which is the
            // condition the expert streamer exists under. A device-local buffer's pointer is not one.
            d.rebindable = is_yes(d.host_buffer);
            // Two more capabilities the device declares for free, both of which decide something
            // above: whether it can wrap memory we already own (what the streamer would need), and
            // whether it can copy while it computes.
            d.host_ptr_buffers = props.caps.buffer_from_host_ptr ? Tri::Yes : Tri::No;
            d.async_copies = props.caps.async ? Tri::Yes : Tri::No;
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
    probe_core_classes(h);

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

void probe_device_support(HardwareProfile & hw, const ModelProfile & model) {
    if (model.expert_type_id < 0 || model.n_expert == 0) return; // nothing to ask about

    // Build the real operation in a context that allocates no tensor data: a MUL_MAT_ID over a 3-D
    // expert tensor of this model's own type and shape. Asking about the actual node is the whole
    // point - a table of "supported formats" would go stale the moment a backend gains a kernel,
    // and would have to be written once per vendor.
    ggml_init_params ip{};
    ip.mem_size = 4 * 1024 * 1024;
    ip.mem_buffer = nullptr;
    ip.no_alloc = true;
    ggml_context * ctx = ggml_init(ip);
    if (!ctx) return;

    const ggml_type type = (ggml_type) model.expert_type_id;
    const int64_t blk = ggml_blck_size(type);
    if (blk > 0) {
        // Shapes only have to be legal and representative; the graph is never run.
        const int64_t n_embd = blk * 8;
        const int64_t n_ff = blk * 8;
        const int64_t n_used = (int64_t) (model.n_expert_used ? model.n_expert_used : 1);

        // Shapes follow ggml_mul_mat_id's own contract: `as` is 3-D with one matrix per expert,
        // `b` is 3-D whose ne[2] is the token count, and `ids` is 2-D [n_expert_used, n_tokens].
        // One token is enough to ask the question, and keeping it at one keeps the assertions
        // trivially satisfied.
        ggml_tensor * w = ggml_new_tensor_3d(ctx, type, n_embd, n_ff, (int64_t) model.n_expert);
        ggml_tensor * x = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, n_embd, 1, 1);
        ggml_tensor * ids = ggml_new_tensor_2d(ctx, GGML_TYPE_I32, n_used, 1);
        ggml_tensor * op = (w && x && ids) ? ggml_mul_mat_id(ctx, w, x, ids) : nullptr;

        if (op) {
            for (ComputeDevice & d : hw.devices) {
                ggml_backend_dev_t dev = ggml_backend_dev_by_name(d.name.c_str());
                if (!dev) continue;
                d.runs_expert_op = ggml_backend_dev_supports_op(dev, op) ? Tri::Yes : Tri::No;

                // A device that executes this layout only out of a buffer type of its own needs the
                // weight converted at load. For a streamed expert that is fatal rather than merely
                // costly: the streamer's whole mechanism is rebinding `data` onto the file's native
                // layout, and a repack is precisely what replaces that layout.
                // Ask about the buffer type this weight would ACTUALLY live in. For a streamed
                // expert that is the host buffer where one is offered, not the device's default:
                // a discrete accelerator's own buffer needs a repack and its host buffer does not,
                // and reading the wrong one of the two is the difference between "this device
                // cannot serve streamed experts" and the truth.
                ggml_backend_buffer_type_t buft = ggml_backend_dev_host_buffer_type(dev);
                if (!buft) buft = ggml_backend_dev_buffer_type(dev);
                d.needs_repack = (buft && !ggml_backend_buft_is_host(buft)) ? Tri::Yes : Tri::No;
            }
        }
    }
    ggml_free(ctx);
}

} // namespace bmoe
