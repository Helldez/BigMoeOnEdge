// What a device COSTS to use, measured by using it.
//
// Two facts live here, and they have nothing in common except that both are usually taken from a
// declaration and both have been wrong when taken that way.
//
// **Is this device's memory the host's?** The device type is not the answer. `TYPE_IGPU` is a claim
// of shared memory, believed because believing it errs safe; `TYPE_GPU` is not the opposite claim —
// Metal reports it on unified-memory hardware. Reading it as "has its own memory" is how one
// physical pool gets counted twice, which is the accounting that told a capacity fitter an
// integrated GPU had 15195 MiB free on a machine holding 11 GB, and took the machine down. So it is
// settled from the native memory topology where available, otherwise by allocating on the device
// and watching what the host loses. A shared topology is not contradicted by a flat allocation delta.
//
// **What does one host/device boundary crossing cost?** This is the term a bandwidth ratio cannot
// see and the one that decides an offload in practice: dense and expert halves alternate per layer,
// so a split placement crosses twice per layer. A device measured at 20 GiB/s against a host's 12
// still ran the move at 0.53x. Measured here by building a chain of cheap ops over a
// hidden-state-sized tensor, running it once on the host and once with alternate ops pinned to the
// device, asking the scheduler how many splits the second one really had, and dividing.
//
// Both are best-effort and both fail closed: where the measurement cannot be made the fact stays
// Unknown or zero, and every rule above declines rather than assuming. Nothing here writes a
// constant into the plan — what is in this file is HOW to ask, never the answer.
//
// Bounded by construction: tensors sized by the model rather than by a guess, samples sized by
// duration rather than by a repetition count, everything released on every path.

#include "bmoe/probe.h"

#include "../io/platform_io.h"
#include "memory_topology.h"

#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace bmoe {

namespace {

using clock_t_ = std::chrono::steady_clock;

double seconds_since(clock_t_::time_point t0) {
    return std::chrono::duration<double>(clock_t_::now() - t0).count();
}

// ── is the device's memory the host's? ───────────────────────────────────────────────────────
//
// Allocate on the device, and see whether the host paid for it. On one pool the host's available
// memory falls by roughly the allocation; on two it does not move.
//
// The test has to survive a noisy signal: `MemAvailable` drifts by hundreds of megabytes on a live
// machine, and has been observed swinging by a gigabyte between consecutive runs on both a phone
// and a desktop. So the allocation is large enough that the answer is not a matter of degree, and
// the verdict is taken on HALF of it — a shared pool loses all of it, a separate one loses none,
// and anything in between is noise rather than a third case.
constexpr uint64_t k_probe_alloc = 256ull << 20;

Tri measure_shared_memory(ggml_backend_dev_t dev) {
    ggml_backend_buffer_type_t buft = ggml_backend_dev_buffer_type(dev);
    if (!buft) return Tri::Unknown;

    pio::ProcessMemory own_before, own_after;
    const bool own_known = pio::process_memory(&own_before);
    const uint64_t before = pio::mem_available_bytes();
    if (before == 0 && !own_known) return Tri::Unknown; // neither ledger is published: nothing to compare

    ggml_backend_buffer_t buf = ggml_backend_buft_alloc_buffer(buft, (size_t) k_probe_alloc);
    if (!buf) return Tri::Unknown;

    // Touch it through the backend so the pages are real. A reservation the device has not backed
    // yet costs the host nothing, and would read as separate memory on a machine that has none.
    void * base = ggml_backend_buffer_get_base(buf);
    if (base) ggml_backend_buffer_clear(buf, 0);

    const bool own_still_known = own_known && pio::process_memory(&own_after);
    const uint64_t after = pio::mem_available_bytes();
    ggml_backend_buffer_free(buf);

    // This process's own ledger is the quiet witness: nobody else writes to it, so pages that
    // appear in it during the allocation are the allocation. It can only say yes - a driver that
    // keeps a shared pool's pages off the process's books leaves it flat, and then the host's
    // figure below is the one left to ask. On a unified-memory desktop the host figure alone gave
    // both answers on consecutive runs, and the wrong one counts a single pool twice.
    //
    // Resident plus swapped, not resident alone: a kernel that compresses or swaps our own idle pages
    // to make room for the allocation leaves the resident figure flat while the charge still grows,
    // and that flat figure is what read a phone's shared GPU as separate memory under pressure.
    const auto charged = [](const pio::ProcessMemory & m) { return m.rss_bytes + m.swap_bytes; };
    if (own_still_known && charged(own_after) >= charged(own_before) + k_probe_alloc / 2) return Tri::Yes;

    if (before == 0 || after == 0) return Tri::Unknown;
    const uint64_t lost = before > after ? before - after : 0;
    return lost >= k_probe_alloc / 2 ? Tri::Yes : Tri::No;
}

// ── what does one boundary crossing cost? ────────────────────────────────────────────────────
//
// A chain of elementwise ops over one hidden-state-sized tensor. The ops are deliberately trivial:
// what is being priced is the CROSSING, so the arithmetic on either side has to be small enough
// that it does not show up in the difference.
constexpr int k_chain_ops = 32;
constexpr double k_sample_seconds = 0.15;
constexpr int k_reps_max = 4096;

struct Chain {
    ggml_context * ctx = nullptr;
    ggml_cgraph * graph = nullptr;
    std::vector<ggml_tensor *> nodes;
    ggml_tensor * input = nullptr;
};

bool build_chain(Chain & c, int64_t n_embd) {
    ggml_init_params ip{};
    ip.mem_size = ggml_tensor_overhead() * (k_chain_ops + 8) + ggml_graph_overhead();
    ip.mem_buffer = nullptr;
    ip.no_alloc = true;
    c.ctx = ggml_init(ip);
    if (!c.ctx) return false;

    c.input = ggml_new_tensor_1d(c.ctx, GGML_TYPE_F32, n_embd);
    if (!c.input) return false;
    ggml_set_name(c.input, "x");

    ggml_tensor * cur = c.input;
    for (int i = 0; i < k_chain_ops; ++i) {
        cur = ggml_scale(c.ctx, cur, 1.0f);
        if (!cur) return false;
        c.nodes.push_back(cur);
    }
    c.graph = ggml_new_graph(c.ctx);
    if (!c.graph) return false;
    ggml_build_forward_expand(c.graph, cur);
    return true;
}

// Run a chain under a scheduler until the sample is long enough to mean something, and return
// seconds per pass. Sized by duration for the same reason every other probe here is: a fixed
// repetition count makes the sample shorter as the machine gets faster, and a two-millisecond
// sample on a general-purpose OS reports scheduling noise.
double time_chain(ggml_backend_sched_t sched, ggml_cgraph * graph) {
    if (ggml_backend_sched_graph_compute(sched, graph) != GGML_STATUS_SUCCESS) return 0.0;
    const auto t0 = clock_t_::now();
    int reps = 0;
    while (reps < k_reps_max) {
        if (ggml_backend_sched_graph_compute(sched, graph) != GGML_STATUS_SUCCESS) return 0.0;
        ++reps;
        if (seconds_since(t0) >= k_sample_seconds) break;
    }
    const double secs = seconds_since(t0);
    return reps > 0 && secs > 0.0 ? secs / reps : 0.0;
}

// The measurement. Returns seconds per crossing, and reports how many crossings the mixed graph
// actually had — which is the scheduler's own count, not ours, because what it decides to split is
// its business and a number we assumed would be a number we invented.
double measure_split_cost(ggml_backend_dev_t dev, ggml_backend_dev_t cpu_dev, int64_t n_embd, uint32_t * splits_out) {
    if (!dev || !cpu_dev || n_embd <= 0) return 0.0;

    ggml_backend_t dev_backend = ggml_backend_dev_init(dev, nullptr);
    ggml_backend_t cpu_backend = ggml_backend_dev_init(cpu_dev, nullptr);
    if (!dev_backend || !cpu_backend) {
        if (dev_backend) ggml_backend_free(dev_backend);
        if (cpu_backend) ggml_backend_free(cpu_backend);
        return 0.0;
    }

    double per_split = 0.0;
    Chain host_chain, mixed_chain;

    // Arm 1: everything on the host. One split, and the baseline the crossing is measured against.
    ggml_backend_t only_cpu[1] = {cpu_backend};
    ggml_backend_sched_t sched_host = ggml_backend_sched_new(only_cpu, nullptr, 1, k_chain_ops + 8, false, false);

    // Arm 2: the same chain with alternate ops pinned to the device, so the scheduler has to hand
    // the tensor across and back repeatedly. `op_offload` stays false: the question is what OUR
    // placement costs, not what the scheduler would do left to itself.
    ggml_backend_t both[2] = {dev_backend, cpu_backend};
    ggml_backend_sched_t sched_mixed = ggml_backend_sched_new(both, nullptr, 2, k_chain_ops + 8, false, false);

    if (sched_host && sched_mixed && build_chain(host_chain, n_embd) && build_chain(mixed_chain, n_embd)) {
        // A device that cannot run this op is not slow at crossing, it simply never crosses; the
        // scheduler would keep everything on the host and the two arms would be the same graph.
        const bool supported = ggml_backend_dev_supports_op(dev, mixed_chain.nodes.front());

        if (supported) {
            for (size_t i = 0; i < mixed_chain.nodes.size(); i += 2)
                ggml_backend_sched_set_tensor_backend(sched_mixed, mixed_chain.nodes[i], dev_backend);

            if (ggml_backend_sched_reserve(sched_host, host_chain.graph) &&
                ggml_backend_sched_reserve(sched_mixed, mixed_chain.graph)) {
                const double t_host = time_chain(sched_host, host_chain.graph);
                const double t_mixed = time_chain(sched_mixed, mixed_chain.graph);
                const int n_splits = ggml_backend_sched_get_n_splits(sched_mixed);

                // One split is a graph that never left its backend: the pinning did not take, and
                // there is nothing here to divide by.
                if (t_host > 0.0 && t_mixed > t_host && n_splits > 1) {
                    if (splits_out) *splits_out = (uint32_t) n_splits;
                    per_split = (t_mixed - t_host) / (double) (n_splits - 1);
                }
            }
        }
    }

    if (host_chain.ctx) ggml_free(host_chain.ctx);
    if (mixed_chain.ctx) ggml_free(mixed_chain.ctx);
    if (sched_host) ggml_backend_sched_free(sched_host);
    if (sched_mixed) ggml_backend_sched_free(sched_mixed);
    ggml_backend_free(dev_backend);
    ggml_backend_free(cpu_backend);
    return per_split;
}

// ── how fast does host memory reach the device's own? ───────────────────────────────────────
//
// One expert at a time, from ordinary pageable memory, into one device buffer, for long enough to
// mean something. That is the copy a device-side expert cache makes for every miss, and its rate is
// neither the link's nominal width nor a pinned-memory benchmark: on a rented card the two differ.
constexpr uint64_t k_upload_buffer = 64ull << 20;

double measure_upload(ggml_backend_dev_t dev, uint64_t granule) {
    ggml_backend_buffer_type_t buft = ggml_backend_dev_buffer_type(dev);
    ggml_backend_t backend = buft ? ggml_backend_dev_init(dev, nullptr) : nullptr;
    if (!backend) return 0.0;
    const uint64_t piece = std::clamp<uint64_t>(granule, 1ull << 16, k_upload_buffer);
    double gibs = 0.0;

    ggml_init_params ip{};
    ip.mem_size = ggml_tensor_overhead() * 2;
    ip.no_alloc = true;
    ggml_context * ctx = ggml_init(ip);
    ggml_tensor * t = ctx ? ggml_new_tensor_1d(ctx, GGML_TYPE_I8, (int64_t) k_upload_buffer) : nullptr;
    ggml_backend_buffer_t buf = t ? ggml_backend_alloc_ctx_tensors_from_buft(ctx, buft) : nullptr;
    std::vector<uint8_t> host(k_upload_buffer, 1);
    if (buf) {
        const uint64_t n_pieces = k_upload_buffer / piece;
        uint64_t moved = 0, i = 0;
        ggml_backend_tensor_set(t, host.data(), 0, piece); // the first copy pays for setting up the path
        ggml_backend_synchronize(backend);
        const auto t0 = clock_t_::now();
        while (seconds_since(t0) < k_sample_seconds) {
            const uint64_t off = (i++ % n_pieces) * piece;
            ggml_backend_tensor_set(t, host.data() + off, off, piece);
            moved += piece;
        }
        ggml_backend_synchronize(backend);
        const double secs = seconds_since(t0);
        if (secs > 0.0 && moved > 0) gibs = (double) moved / secs / (double) (1ull << 30);
        ggml_backend_buffer_free(buf);
    }
    if (ctx) ggml_free(ctx);
    ggml_backend_free(backend);
    return gibs;
}

} // namespace

void probe_device_costs(HardwareProfile & hw, const ModelProfile & model) {
    ggml_backend_dev_t cpu_dev = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);

    for (ComputeDevice & d : hw.devices) {
        if (d.is_cpu) continue;        // the host is not a device to cross to
        if (!d.probe_here()) continue; // asked in another process, or found unusable there
        ggml_backend_dev_t dev = ggml_backend_dev_by_name(d.name.c_str());
        if (!dev) continue;

        // Settle the memory question first: it is cheap, and a device whose memory turns out to be
        // the host's changes what every capacity rule above may conclude.
        // Native topology and an integrated-device declaration are positive evidence. Allocator
        // reuse or compression can flatten this delta, but cannot make their pool separate.
        if (d.shares_host_memory != Tri::Yes)
            d.shares_host_memory = detail::refine_memory_sharing(d.shares_host_memory, measure_shared_memory(dev));

        // Do not take a quarter of a gigabyte from a device that is short of it just to learn what
        // a crossing costs; the answer is worth having and it is not worth a failed allocation.
        if (d.has_own_memory() && d.memory_free && d.memory_free < k_probe_alloc * 4) continue;

        uint32_t splits = 0;
        const double per_split = measure_split_cost(dev, cpu_dev, (int64_t) model.n_embd, &splits);
        if (per_split > 0.0) {
            d.split_seconds = per_split;
            d.graph_splits = splits;
        }
        // Only where there is a second pool to reach: on a shared one a miss is a copy within it.
        const uint64_t expert = model.expert_slice_bytes * std::max<uint32_t>(1, model.n_expert_projections);
        if (d.has_own_memory() && expert > 0) d.upload_gibs = measure_upload(dev, expert);
    }
}

} // namespace bmoe
