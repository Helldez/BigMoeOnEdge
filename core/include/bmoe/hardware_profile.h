// What this machine is, as facts a planning rule may read.
//
// The whole point of this header is that it contains no platform names. Windows, Android, iOS,
// CUDA and Metal appear only in the adapters that FILL a HardwareProfile, never in the rules that
// consume one — so a rule can be unit-tested against a synthetic machine, and a new platform is a
// new adapter rather than a new branch. If a rule finds itself needing to know which platform it
// is on, the profile is missing a field; add the field, not the branch.
//
// Every probed fact is a tri-state. `Unknown` is not a defect to be papered over with a default:
// it is the input that makes "decline instead of guessing" mechanical, because a rule that has no
// fact leaves its knob where it was and says so in the plan's rationale.
//
// Pure policy: no llama.cpp, no OS headers, no I/O. See core/src/plan/hardware_probe.cpp for the
// adapter that measures these, and bmoe/planner.h for what reads them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bmoe {

// A probed yes/no. Absent by default, because a profile that was never filled must not read as a
// machine where everything is false.
enum class Tri { Unknown, No, Yes };

inline bool is_yes(Tri t) {
    return t == Tri::Yes;
}
inline bool is_known(Tri t) {
    return t != Tri::Unknown;
}

// What happens to bytes we are holding when the machine wants the memory back. This is the single
// fact behind the dense-residency policy, and it is the axis on which platforms genuinely differ:
// the same anonymous allocation is cheap to lose where it is compressed, expensive where it is
// written to a disk, and fatal where the kernel answers pressure by killing the process.
enum class Overflow {
    Unknown,  // not probed; a rule that depends on this must decline rather than assume
    None,     // nothing can take it back (a reclaim-exempt allocation)
    Compress, // compressed in RAM; a touch costs a minor fault plus decompression, invisible to I/O counters
    Swap,     // written out to a swap device; slow, but the process survives
    Refault,  // dropped, and re-read from the file it came from on the next touch
    Kill,     // the process is terminated for exceeding its share (iOS jetsam)
};

// Where a headroom figure came from. Carried for the rationale, so a reader can weigh the number:
// an estimate from what the compressor is currently achieving is worth less than a measurement, and
// both are worth more than the reported budget. No rule branches on this; rules read the bytes.
enum class Headroom {
    Unknown,      // not probed: rules fall back to the reported residency budget
    Compressible, // derived from the machine's own compression ratio and its reclaimable set
    Measured,     // held until the kernel took pages back, and the boundary observed
};

// A compute device the graph could run on, as ggml reports it. Kept deliberately thin: the planner
// only ever asks "how much memory does it have of its own, and can we address it from the host".
struct ComputeDevice {
    std::string name;        // backend-reported name, for the rationale only; never matched against
    std::string description; //
    uint64_t memory_free = 0;
    uint64_t memory_total = 0;
    // True when this device IS the CPU backend. Kept separate from `host_memory` on purpose: an
    // integrated accelerator also reads host memory, and treating the two as one is how an iGPU
    // gets silently excluded from a question that was only ever meant to exclude the CPU.
    bool is_cpu = false;

    // True when the device's memory IS host memory (an integrated GPU, unified memory). Moving a
    // tensor off such a device frees nothing, which is why the capacity tier is nearly inert there
    // and only the bandwidth tier is left.
    bool host_memory = false;
    // True when this device can execute over a HOST buffer - pinned memory it reads directly rather
    // than a buffer of its own. It is the property that decides whether streamed experts can be
    // computed here at all, and it is not the same as having host memory: a discrete accelerator
    // across a link can offer one too, and then a weight we read from flash is both writable by us
    // and readable by it. Unknown until asked.
    Tri host_buffer = Tri::Unknown;

    // True when this device can wrap memory the CALLER already owns in a buffer of its own. It is
    // the capability the expert streamer would need, and it is not the same as offering a host
    // buffer: one hands us memory the device can read, the other reads memory we hand it. The
    // streamer allocates its own reservations and rebinds tensors onto them, so a device that can
    // only do the first still cannot see the bytes a streamed expert is actually made of.
    Tri host_ptr_buffers = Tri::Unknown;

    // True when this device can copy while it computes. On a device across a link it is the
    // difference between a transfer that costs its own time and one that hides behind the previous
    // operation - a different axis from our own read/compute overlap, which hides compute behind I/O.
    Tri async_copies = Tri::Unknown;

    // True when a weight placed here can still have its `data` pointer rebound by us — the whole
    // mechanism of the expert streamer. Host memory qualifies, and so does a host buffer offered by
    // a device with memory of its own; a device-local buffer does not, because its pointer is not a
    // host address and nothing we read can be written into it.
    bool rebindable = false;

    // Whether this device can execute the model's own expert matmul on the model's own quantized
    // layout. Probed by asking the backend about the real operation rather than by listing formats,
    // so a backend that gains a kernel answers differently without anything here changing.
    // Unknown until asked; a rule that needs it declines.
    Tri runs_expert_op = Tri::Unknown;

    // Whether placing a weight here requires the loader to repack it into a device-specific layout.
    // A repacked expert tensor cannot be served by the streamer at all: the whole mechanism is
    // rebinding `data` onto the file's native layout, and a repack replaces exactly that.
    Tri needs_repack = Tri::Unknown;

    // Memory bandwidth in GiB/s, 0 when unmeasured. This is the number that decides a decode
    // offload, because batch-1 decode is a chain of GEMVs that reads every weight once for a single
    // multiply-accumulate: it is bound by bandwidth, not by arithmetic. A device that shares the
    // host's memory therefore has nothing to win however fast its ALUs are, and one with memory of
    // its own wins in proportion to this.
    double memory_bandwidth_gibs = 0.0;
};

// The measured read rate at one (request size, lane count) point. A curve of these is the only
// honest way to answer "how many lanes" and "is our request size in the punished regime", because
// the answer differs by an order of magnitude between storage classes: a 4 KiB read returns 2% of
// peak on one desktop SSD and 6.8% on a phone's UFS.
struct RateSample {
    uint32_t request_bytes = 0;
    uint32_t lanes = 0;
    double mib_per_s = 0.0;
};

// The storage the model file lives on, and how it behaves when we read it the way we read it.
struct StorageFacts {
    uint32_t align = 0; // block size uncached reads must be aligned to; 0 when unknown

    // Whether cache-bypassing reads actually deliver correct bytes on THIS path. It is a per-path
    // fact, not a per-platform one: the same phone answers yes on one filesystem and silently wrong
    // on another, which is why the engine verifies rather than trusting the open's success.
    Tri direct_ok = Tri::Unknown;

    // Whether a live mapping of the file serialises our concurrent uncached reads. Measured, never
    // inferred from the platform: it is true on one desktop filesystem and false on a phone's, and
    // the difference is worth 46% of throughput.
    Tri mapping_serialises_reads = Tri::Unknown;

    std::vector<RateSample> rate_curve; // empty when unprobed

    // The two comparable points behind mapping_serialises_reads, in MiB/s, 0 when not measured.
    // Kept so the rationale can quote them: a verdict a reader can check beats one to be believed.
    double rate_mapped_mibs = 0.0;
    double rate_unmapped_mibs = 0.0;

    // Best measured rate at or near `request_bytes` for `lanes`, or 0 when the curve says nothing
    // about that point. Nearest-sample lookup on purpose: interpolating a curve whose whole shape
    // is a latency floor would invent throughput between the samples.
    double rate_at(uint32_t request_bytes, uint32_t lanes) const;

    // The lane count with the highest measured rate at `request_bytes`, or 0 when unprobed. This is
    // the only source of a lane number that is not a constant carried over from one device.
    uint32_t best_lanes(uint32_t request_bytes) const;
};

struct HardwareProfile {
    // ── memory ──────────────────────────────────────────────────────────────────────
    // How many bytes THIS PROCESS may hold resident. On most systems that is the kernel's own
    // "available" figure; where a process has a share of its own it is that share instead. The
    // distinction matters: sizing from system-wide availability is meaningless under a per-process
    // limit, and it is the one number every sizing rule here reads.
    uint64_t residency_budget = 0;
    uint64_t memory_total = 0; // for the rationale, and for margins expressed as a fraction

    // What happens to our anonymous buffers under pressure. Decides the dense policy on its own.
    Overflow anon_overflow = Overflow::Unknown;

    // How many bytes this process can hold and expect to KEEP. It is a different question from
    // `residency_budget`, and on a machine whose reclaim compresses it has a different answer in
    // both directions: the kernel will compress other processes' idle pages to make room for us, so
    // the reported figure is a floor rather than a cap; and it will just as readily take ours back,
    // so a set that merely fits is not a set that survives. 0 means unmeasured, and every rule that
    // would read it then falls back to the reported budget and says which one it used.
    uint64_t holdable_bytes = 0;
    Headroom holdable_from = Headroom::Unknown;

    // Largest single reclaim-exempt allocation available, 0 where the platform has none. A store
    // the kernel may not take back is the only way to keep the dense set out of a compressed swap.
    uint64_t reclaim_exempt_max = 0;

    // Whether clean file-backed pages count against `residency_budget`. Where they do not, leaving
    // a weight mmap'd is free of the budget entirely — which inverts the dense policy on a platform
    // that kills on resident size.
    Tri file_pages_counted = Tri::Unknown;

    // ── compute ─────────────────────────────────────────────────────────────────────
    uint32_t n_cores = 0; // 0 when unknown

    // The core classes this machine has, fastest first, as counts. A heterogeneous processor is not
    // described by a core count: a barrier waits for the slowest participant, so adding cores from a
    // slower class can cost throughput rather than add it. One entry means every core is alike.
    std::vector<uint32_t> core_classes;

    // The thread count that reached the highest rate on this machine's own memory-bound matmul,
    // 0 when unmeasured. It is a measurement rather than a rule for a reason: the rule that read
    // core classes instead - fast class only, because a barrier waits for its slowest participant -
    // was refuted by the first heterogeneous machine it met, where it cost 37% of throughput. How
    // many threads a machine wants is a property of that machine, and the honest way to have it is
    // to ask the machine.
    uint32_t best_threads = 0;

    // How fast the HOST consumes weight bytes in THIS MODEL's own format, in GiB/s, 0 when
    // unmeasured. Not raw memory bandwidth, and the difference is the point: a quantized matmul
    // carries dequantisation work per byte, so it reaches a different rate and scales differently
    // with threads than an F32 read of the same memory would. Paired with a device's figure from the
    // same graph, it answers the only question that decides an offload - who consumes this model's
    // weights faster - which is what batch-1 decode is, every weight read once and multiplied once.
    double host_bandwidth_gibs = 0.0;

    std::vector<ComputeDevice> devices;

    // ── storage ─────────────────────────────────────────────────────────────────────
    StorageFacts storage;

    // Free-text label for the rationale and the CSV header. Never parsed, never matched: it exists
    // so a committed benchmark says which machine it came from, not so a rule can read it.
    std::string label;

    // Total memory of every device that is NOT host memory. This is the only capacity the upstream
    // capacity fitter can actually buy with, so it is also the test for whether that tier is worth
    // running at all on this machine.
    uint64_t device_local_memory() const;
};

} // namespace bmoe
