// How fast each compute engine can pull weights out of the memory it will read them from.
//
// This is the number that decides an offload, and it is the only one that does. Batch-1 decode is a
// chain of GEMVs: every weight is read once and multiplied once, so arithmetic is never the
// constraint and the winner is whoever moves bytes faster. On a machine with separate memories that
// is a property of each memory. On a machine with ONE memory it is a property of the engine's path
// to it — and the two are not equal, which is the whole reason this probe exists: cores reach a
// fraction of a fabric that an accelerator on the same die saturates.
//
// The measurement is one graph, run on every backend. That matters more than what the graph is: a
// ratio between a hand-rolled loop on one side and a vendor kernel on the other would measure the
// two implementations rather than the two paths, and the number is only ever used as a ratio. So
// the same `mul_mat` over the same buffer is scheduled on the CPU backend and on each device, and
// what comes back is bytes read over seconds, in the same units, from the same work.
//
// Bounded by construction: a buffer sized to outrun any last-level cache and nothing more, a few
// repetitions, everything released before returning. It costs a fraction of the load it runs
// inside, and where a backend will not allocate or will not run the op, that device simply keeps
// its unmeasured 0 and every rule above declines rather than assuming.

#include "bmoe/probe.h"

#include "ggml-backend.h"
#include "ggml.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace bmoe {

namespace {

// Big enough that the read comes from memory rather than from a cache, small enough to allocate
// anywhere this runs. A GEMV over a matrix this size reads every byte of it exactly once.
constexpr int64_t k_rows = 4096;
constexpr int64_t k_cols = 4096;
constexpr int k_reps = 4;

// The weight type to measure with. The model's own where it has one, because a quantized matmul and
// an F32 one do not scale the same way with threads: the F32 kind is pure bandwidth and stops
// improving as soon as the bus is full, while the quantized kind carries dequantisation per byte
// and keeps using cores past that. Falling back to F32 makes the figure a bandwidth number and says
// so; using the model's makes it a number about this workload.
ggml_type weight_type(const ModelProfile & model) {
    if (model.expert_type_id < 0) return GGML_TYPE_F32;
    const ggml_type t = (ggml_type) model.expert_type_id;
    return ggml_blck_size(t) > 0 ? t : GGML_TYPE_F32;
}

// The thread counts worth trying. Powers of two up to the core count, plus the core count itself:
// enough resolution to separate "half the cores" from "all of them", which is where the refuted
// rule went wrong, without turning a probe into a benchmark.
std::vector<int> thread_ladder(uint32_t n_cores) {
    std::vector<int> out;
    for (int t = 1; t <= (int) n_cores; t *= 2)
        out.push_back(t);
    if (n_cores > 0 && (out.empty() || out.back() != (int) n_cores)) out.push_back((int) n_cores);
    return out;
}

// Run the same GEMV on one backend and return the rate at which it read the weight, in GiB/s.
// Returns 0 for every way of not knowing: no backend, no allocation, no kernel, no time elapsed.
// A zero here is "unmeasured", and it is never to be read as "slow".
double measure(ggml_backend_dev_t dev, ggml_type wtype, int n_threads = 0) {
    if (!dev) return 0.0;
    ggml_backend_t backend = ggml_backend_dev_init(dev, nullptr);
    if (!backend) return 0.0;

    // Ask the backend for its own thread setter rather than calling a CPU-specific function: it is
    // how ggml exposes every backend-specific entry point, so a backend that has one is configured
    // and one that has none simply runs as it is.
    if (n_threads > 0) {
        ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(dev);
        auto set_threads =
            reg ? (ggml_backend_set_n_threads_t) ggml_backend_reg_get_proc_address(reg, "ggml_backend_set_n_threads")
                : nullptr;
        if (set_threads) set_threads(backend, n_threads);
    }

    double gibs = 0.0;
    ggml_init_params ip{};
    ip.mem_size = ggml_tensor_overhead() * 8 + ggml_graph_overhead();
    ip.mem_buffer = nullptr;
    ip.no_alloc = true;
    ggml_context * ctx = ggml_init(ip);
    ggml_backend_buffer_t buf = nullptr;

    if (ctx) {
        // Shapes must respect the type's block size, which is what makes this the model's own kind
        // of matmul rather than a shape the kernel would refuse.
        const int64_t blk = ggml_blck_size(wtype);
        const int64_t cols = blk > 0 ? (k_cols / blk) * blk : k_cols;
        ggml_tensor * w = ggml_new_tensor_2d(ctx, wtype, cols, k_rows);
        ggml_tensor * x = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, cols, 1);
        ggml_tensor * y = (w && x) ? ggml_mul_mat(ctx, w, x) : nullptr;

        // Ask before allocating: a backend without a kernel for this shape would otherwise be
        // charged for a fallback that says nothing about its own path to memory.
        if (y && ggml_backend_dev_supports_op(dev, y)) {
            buf = ggml_backend_alloc_ctx_tensors(ctx, backend);
            if (buf) {
                // Any bytes will do - the rate does not depend on the values - but they must be
                // written through the backend, since the buffer may not be host memory.
                std::vector<char> zeros((size_t) std::max(ggml_nbytes(w), ggml_nbytes(x)), 0);
                ggml_backend_tensor_set(w, zeros.data(), 0, ggml_nbytes(w));
                ggml_backend_tensor_set(x, zeros.data(), 0, ggml_nbytes(x));

                ggml_cgraph * gf = ggml_new_graph(ctx);
                ggml_build_forward_expand(gf, y);

                ggml_backend_graph_compute(backend, gf); // once to warm anything that warms
                const auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < k_reps; ++i)
                    ggml_backend_graph_compute(backend, gf);
                ggml_backend_synchronize(backend);
                const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

                if (secs > 0.0) {
                    const double bytes = (double) ggml_nbytes(w) * k_reps;
                    gibs = bytes / secs / (1024.0 * 1024.0 * 1024.0);
                }
            }
        }
    }

    if (buf) ggml_backend_buffer_free(buf);
    if (ctx) ggml_free(ctx);
    ggml_backend_free(backend);
    return gibs;
}

} // namespace

void probe_bandwidth(HardwareProfile & hw, const ModelProfile & model) {
    const ggml_type wtype = weight_type(model);
    const uint64_t needed = (uint64_t) k_rows * k_cols * sizeof(float);
    for (ComputeDevice & d : hw.devices) {
        ggml_backend_dev_t dev = ggml_backend_dev_by_name(d.name.c_str());
        if (!dev) continue;
        // Do not take memory a device may be short of just to learn how fast it is. A device with
        // its own memory is skipped unless it has room to spare; the answer is worth having, and it
        // is not worth pushing a machine into a failed allocation to get. Host memory is not
        // guarded here because the residency budget already governs it.
        if (!d.host_memory && d.memory_free && d.memory_free < needed * 4) continue;
        const double gibs = measure(dev, wtype);
        if (gibs <= 0.0) continue;
        d.memory_bandwidth_gibs = gibs;
        // The host's own figure is whatever the CPU backend reached on the same graph. Taking it
        // from the same measurement rather than from a separate loop is the point: the comparison
        // the rules make is a ratio, and a ratio between two different experiments means nothing.
        if (ggml_backend_dev_type(dev) != GGML_BACKEND_DEVICE_TYPE_CPU) continue;
        hw.host_bandwidth_gibs = gibs;

        // How many threads this machine wants, asked rather than reasoned about. The same graph at
        // each rung of the ladder; the rung with the highest rate wins, and ties go to the smaller
        // count because threads are not free elsewhere. What this instrument sees is a memory-bound
        // matmul, which is what a batch-1 decode mostly is - not the whole of it, and the plan says
        // as much rather than presenting the number as a decode measurement.
        double best_rate = 0.0;
        uint32_t best_n = 0;
        for (int t : thread_ladder(hw.n_cores)) {
            const double r = measure(dev, wtype, t);
            if (r > best_rate * 1.02) { // a rung has to beat the incumbent by more than noise
                best_rate = r;
                best_n = (uint32_t) t;
            }
        }
        if (best_n > 0) {
            hw.best_threads = best_n;
            hw.host_bandwidth_gibs = std::max(hw.host_bandwidth_gibs, best_rate);
        }
    }

    // Put it where a reader will see it. The label is the machine's one line of free text, printed
    // above the plan and carried into a benchmark's CSV header, and a figure that decides an
    // offload belongs in the description of the machine rather than only inside a rule that read it.
    if (hw.host_bandwidth_gibs > 0.0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), ", %.0f GiB/s on this model's matmul", hw.host_bandwidth_gibs);
        hw.label += buf;
    }
}

} // namespace bmoe
