// The planning rules. See bmoe/planner.h for the contract and the two invariants.
//
// Read this as a table rather than as code: each block below is one knob, the fact that decides it,
// and what happens when that fact is missing. Nothing here reads a platform, an architecture or a
// tensor name — the inputs carry no such thing — so the only way to add hardware is to teach an
// adapter to measure one more fact.

#include "bmoe/planner.h"

#include <algorithm>
#include <cstdio>

namespace bmoe {

namespace {

uint64_t mib(uint64_t bytes) {
    return bytes >> 20;
}

std::string u64s(uint64_t v) {
    return std::to_string((unsigned long long) v);
}

std::string dense_mode_name(DenseWeightsMode m) {
    switch (m) {
    case DenseWeightsMode::Mmap:
        return "mmap";
    case DenseWeightsMode::Warmed:
        return "warm";
    case DenseWeightsMode::Anonymous:
        return "anon";
    case DenseWeightsMode::Pinned:
        return "ahwb";
    }
    return "?";
}

// What this process may hold. The measured figure wins where it exists, because it answers the
// question the rules are actually asking - what can be KEPT - while the reported one answers what
// could be allocated at this instant. They differ by more than rounding wherever a reclaim
// compresses: the reported number is a floor there, and sizing from it leaves memory unused.
uint64_t budget_bytes(const HardwareProfile & hw) {
    return hw.holdable_bytes ? hw.holdable_bytes : hw.residency_budget;
}

// How much of the residency budget to leave for everything that is not us. The fraction is chosen
// by what losing memory costs here, which is the honest axis: it is cheap where a reclaim only
// compresses, and fatal where exceeding the share ends the process.
uint64_t margin_bytes(const HardwareProfile & hw, const PlannerPolicy & pol) {
    float frac = pol.margin_when_unknown;
    switch (hw.anon_overflow) {
    case Overflow::Compress:
        frac = pol.margin_when_compressed;
        break;
    case Overflow::Swap:
        frac = pol.margin_when_swapped;
        break;
    case Overflow::Kill:
        frac = pol.margin_when_killed;
        break;
    case Overflow::None:
    case Overflow::Refault:
    case Overflow::Unknown:
        break;
    }
    const uint64_t by_frac = (uint64_t) ((double) budget_bytes(hw) * frac);
    return std::max(by_frac, pol.margin_min_bytes);
}

// Whether taking resident pages back from us is CHEAP for this kernel, which is the case where
// fitting stops implying being left alone. It is the narrow reading on purpose. Where a reclaim
// only compresses or only drops a clean page, the kernel does it routinely and to anyone, so a
// model with nothing to spare is reclaimed from underneath continuously - that is the case we
// measured. Where a reclaim has to write to a disk the kernel is far more reluctant, where the
// allocation is exempt nothing can take it, and where the limit is a hard cap nothing is taken at
// all. And `Unknown` keeps the default like every other missing fact: the evidence here is specific
// to cheap reclaim, so an unprofiled machine is not entitled to its conclusion. The headroom probe
// is what generalises this from a class of kernel to a measurement.
bool reclaim_is_cheap(Overflow o) {
    switch (o) {
    case Overflow::Compress:
    case Overflow::Refault:
        return true;
    case Overflow::None:
    case Overflow::Swap:
    case Overflow::Kill:
    case Overflow::Unknown:
        break;
    }
    return false;
}

// Where the budget came from, in the words the rationale prints. The distinction matters to a
// reader deciding how much to trust a plan: an estimate and a measurement are not the same claim.
const char * headroom_name(Headroom h) {
    switch (h) {
    case Headroom::Compressible:
        return "estimated from what this machine's compressor is achieving";
    case Headroom::Measured:
        return "measured by holding memory until the machine took some back";
    case Headroom::Unknown:
        break;
    }
    return "as reported available, unmeasured";
}

const char * overflow_name(Overflow o) {
    switch (o) {
    case Overflow::None:
        return "nothing can reclaim it";
    case Overflow::Compress:
        return "reclaimed into compressed RAM";
    case Overflow::Swap:
        return "swapped to disk";
    case Overflow::Refault:
        return "dropped and re-read from the file";
    case Overflow::Kill:
        return "the process is killed";
    case Overflow::Unknown:
        break;
    }
    return "unprobed";
}

} // namespace

const char * source_name(Source s) {
    switch (s) {
    case Source::Measured:
        return "measured";
    case Source::Derived:
        return "derived";
    case Source::Policy:
        return "policy";
    case Source::Operator:
        return "operator";
    case Source::Unprobed:
        return "unprobed";
    }
    return "?";
}

const char * regime_name(Regime r) {
    switch (r) {
    case Regime::Unknown:
        return "unknown";
    case Regime::NotMoe:
        return "not-moe";
    case Regime::Fits:
        return "fits";
    case Regime::ExpertsStream:
        return "experts-stream";
    case Regime::DenseOversized:
        return "dense-oversized";
    }
    return "?";
}

bool PlanRequest::is_pinned(const char * knob) const {
    for (const std::string & p : pinned)
        if (p == knob) return true;
    return false;
}

Plan plan_run(const RunConfig & base, const HardwareProfile & hw, const ModelProfile & model, const PlanRequest & req) {
    return plan_run(base, hw, model, Placement{}, req, PlannerPolicy::defaults());
}

Plan plan_run(const RunConfig & base,
              const HardwareProfile & hw,
              const ModelProfile & model,
              const Placement & placement,
              const PlanRequest & req) {
    return plan_run(base, hw, model, placement, req, PlannerPolicy::defaults());
}

Plan plan_run(const RunConfig & base,
              const HardwareProfile & hw,
              const ModelProfile & model,
              const Placement & placement,
              const PlanRequest & req,
              const PlannerPolicy & pol) {
    Plan p;
    p.config = base;
    p.placement = placement;

    // What the first stage left for the second. Without a placement every layer's experts are on
    // the host and nothing of the model sits on a device; with one, only the host share of the
    // experts is the streamer's to serve, and the dense set the fitter kept on the host is what it
    // costs the residency budget.
    // Expert bytes prorate over the layers that CARRY experts, not over every block in the file.
    // On an architecture with leading dense blocks the two counts differ, and dividing the expert
    // set by the larger of them understates what the host still has to stream.
    const uint32_t n_moe_layer = model.n_moe_layer;
    const uint32_t host_layers = placement.fitted ? (uint32_t) placement.host_expert_layers.size() : n_moe_layer;
    const uint64_t host_expert_bytes = (placement.fitted && n_moe_layer)
                                           ? (uint64_t) ((double) model.expert_bytes * host_layers / n_moe_layer)
                                           : model.expert_bytes;

    auto note = [&](const char * knob, std::string value, Source src, std::string reason) {
        Decision d;
        d.knob = knob;
        d.value = std::move(value);
        d.source = src;
        d.reason = std::move(reason);
        p.decisions.push_back(std::move(d));
    };

    // A knob the caller set is a knob we do not own. Records it and reports that it is off-limits.
    auto pinned = [&](const char * knob, std::string value) {
        if (!req.is_pinned(knob)) return false;
        note(knob, std::move(value), Source::Operator, "set by the caller; the planner leaves it alone");
        return true;
    };

    auto decline = [&](std::string why) {
        p.streaming_declined = true;
        p.decline_reason = std::move(why);
        p.config.moe.enabled = false;
    };

    // ── the regime: a ratio of the two profiles, never a property of either ─────────
    if (!model.ok) {
        p.regime = Regime::Unknown;
        decline("the model file could not be read, so nothing about it could be planned");
        note("moe-stream", "off", Source::Unprobed, "model profile unavailable");
        return p;
    }
    if (!model.is_moe) {
        p.regime = Regime::NotMoe;
        decline("this model has no routed experts: there is nothing for the expert streamer to do");
        note("moe-stream", "off", Source::Derived, "the file carries no expert tensors");
        return p;
    }
    if (hw.residency_budget == 0) {
        p.regime = Regime::Unknown;
        decline("this machine reported no residency budget, so no size could be derived from it");
        note("moe-stream", "off", Source::Unprobed, "machine not profiled");
        return p;
    }

    const uint64_t margin = margin_bytes(hw, pol);
    const uint64_t holdable = budget_bytes(hw);
    const uint64_t usable = holdable > margin ? holdable - margin : 0;

    // The bytes that would have to be resident on the host if nothing were streamed: with a
    // placement, what the fitter left here; without one, the whole file.
    //
    // On a machine where every device's memory IS the host's, what the fitter put "on a device" is
    // in this same pool and has to be charged to it. The fitter's own arithmetic treats the two as
    // separate - it was told an integrated accelerator had 15 GB free on an 11 GB machine - and a
    // budget that inherits that mistake will size a cache for memory that is already spoken for.
    // Where devices have memory of their own the term is zero and nothing changes.
    bool devices_share_host_memory = !hw.devices.empty();
    for (const ComputeDevice & d : hw.devices)
        if (!d.host_memory) devices_share_host_memory = false;
    const uint64_t device_on_host = devices_share_host_memory ? placement.device_bytes : 0;

    const uint64_t host_all =
        placement.fitted ? placement.host_resident_bytes + host_expert_bytes + device_on_host : model.file_bytes;
    const uint64_t host_dense = placement.fitted ? placement.host_resident_bytes + device_on_host : model.dense_bytes;
    if (host_all <= usable) {
        p.regime = Regime::Fits;
    } else if (host_dense <= usable) {
        p.regime = Regime::ExpertsStream;
    } else {
        p.regime = Regime::DenseOversized;
    }

    // Fitting is not the same as being left alone. Where the machine can take pages back, residency
    // has to clear a second bar: room beyond the model itself, so its weights are not reclaimed and
    // refaulted a page at a time under whatever else the machine is doing. The bar is a policy ratio
    // rather than a measurement, and the reason it can be this crude is that the error is asymmetric
    // by two orders of magnitude: streaming a model that would have fitted costs some reads, while
    // residency on a model that does not is 0.1 tok/s against 5.0. Until the headroom probe exists,
    // the cheap side of that asymmetry is the right default, and a caller who disagrees drops --auto.
    const uint64_t air = usable > host_all ? usable - host_all : 0;
    const uint64_t air_needed = (uint64_t) ((double) host_all * (double) pol.fits_air_ratio);
    const bool air_short = p.regime == Regime::Fits && reclaim_is_cheap(hw.anon_overflow) && air < air_needed;
    if (air_short) p.regime = host_dense <= usable ? Regime::ExpertsStream : Regime::DenseOversized;

    if (placement.fitted) {
        note("placement", u64s((uint64_t) std::max(0, placement.n_gpu_layers)) + " layers on devices", Source::Measured,
             placement.outcome + "; " + u64s(host_layers) + " of " + u64s(n_moe_layer) +
                 " MoE layers keep their experts on the host, host-resident " +
                 u64s(mib(placement.host_resident_bytes)) + " MiB, on devices " + u64s(mib(placement.device_bytes)) +
                 " MiB, context " + u64s(placement.n_ctx) + " (fitter's host breakdown: model " +
                 u64s(mib(placement.raw_host_model_bytes)) + ", context " +
                 u64s(mib(placement.raw_host_context_bytes)) + ", compute " +
                 u64s(mib(placement.raw_host_compute_bytes)) + " MiB; file " + u64s(mib(model.file_bytes)) +
                 ", experts " + u64s(mib(model.expert_bytes)) + ")");
        if (placement.n_ctx && !req.is_pinned("ctx-size")) {
            p.config.n_ctx = (int) placement.n_ctx;
            note("ctx-size", u64s(placement.n_ctx), Source::Measured,
                 "the capacity fitter shrinks context before it moves weights; this is where it settled");
        }
        p.config.n_gpu_layers = placement.n_gpu_layers;
        p.config.buft_overrides = placement.override_patterns;
    } else {
        note("placement", "none", placement.shared_memory_placement ? Source::Derived : Source::Unprobed,
             placement.shared_memory_placement
                 ? "the capacity fitter ran and its answer was set aside: every device here reads this "
                   "host's own memory, so 'what fits where' has one pool and no answer. Everything is on "
                   "the host, which is what it already was"
                 : "the capacity fitter was not run, so no layer is placed on a device: everything is on "
                   "the host");
    }

    note("regime", regime_name(p.regime), Source::Derived,
         "on the host " + u64s(mib(host_all)) + " MiB (dense " + u64s(mib(host_dense)) + ") against " +
             u64s(mib(usable)) + " MiB usable, which is the " + u64s(mib(holdable)) + " MiB this process may hold (" +
             headroom_name(hw.holdable_from) + ") less a " + u64s(mib(margin)) + " MiB margin (" +
             overflow_name(hw.anon_overflow) + ")");

    if (air_short)
        note("residency", "not verified", Source::Derived,
             "the host set fits with " + u64s(mib(air)) + " MiB to spare against the " + u64s(mib(host_all)) +
                 " MiB it would hold, and a reclaim is cheap here (" + overflow_name(hw.anon_overflow) +
                 "): a model that fits only just is reclaimed from underneath and refaults its weights one page at "
                 "a time, so the experts stream instead. Run without --auto to keep plain residency");

    if (placement.fitted && host_layers == 0) {
        p.regime = Regime::Fits;
        decline("the capacity fitter placed every layer's experts on a device; nothing is left on the host for "
                "the streamer to serve");
        note("moe-stream", "off", Source::Measured, "no expert tensor remains on the host");
        return p;
    }
    if (p.regime == Regime::Fits) {
        decline("what is left on the host fits in this machine's residency budget; streaming it from flash "
                "would only add reads that resident weights do not need");
        note("moe-stream", "off", Source::Derived,
             placement.fitted ? "the host residual fits: llama.cpp's placement is the whole plan"
                              : "the model fits: hand the placement to llama.cpp instead");
        return p;
    }

    // ── the accelerator axis: what may leave the host BEYOND the fitter's placement ─
    // Read the `placement` decision above first: layers the fitter put on a device are already
    // there, computed there, and none of this touches them. What follows is only about the residue
    // the fitter left on the host - the experts this engine streams - and whether anything more can
    // be done with them than running them on the CPU cores.
    // Two facts settle this, and neither is a vendor name. A device whose memory IS the host's has
    // no bandwidth of its own to win, and batch-1 decode is a chain of GEMVs that reads every weight
    // once for a single multiply-accumulate: it is bandwidth-bound, so on such a device an offload
    // moves the work without moving the bottleneck. A device with memory of its own wins in
    // proportion to that memory's bandwidth, which is a number this profile does not yet carry.
    //
    // The streamed experts are separately constrained, and absolutely: the streamer serves a tensor
    // by rebinding `data` onto the file's native layout, so a weight it serves must live in a host
    // buffer that is not repacked. A device that only executes a repacked layout is excluded here
    // by that property rather than by name, which is how an NPU with two native quant formats ends
    // up excluded without a single line written for it.
    {
        uint64_t device_local = 0;
        const ComputeDevice * candidate = nullptr;
        for (const ComputeDevice & d : hw.devices) {
            if (d.host_memory) continue;
            device_local += d.memory_total;
            if (!candidate) candidate = &d;
        }

        if (hw.devices.empty()) {
            note("extra-offload", "none", Source::Unprobed,
                 "no compute devices were enumerated (the backends register at load), so there was nothing "
                 "to consider beyond what the fitter already placed");
        } else if (device_local == 0) {
            // Two claims used to be made here and only one of them was provable. That moving a
            // weight onto a device whose memory IS the host's frees nothing is arithmetic. That it
            // "wins no bandwidth" was an assumption, and a wrong one: on shared memory an
            // accelerator can still reach more of the bus than the cores do - which is measured
            // elsewhere at roughly twice - because a CPU core count is not a memory controller. So
            // the capacity half is stated and the bandwidth half is compared, or admitted unknown.
            const ComputeDevice * fastest = nullptr;
            for (const ComputeDevice & d : hw.devices) {
                if (d.is_cpu || d.memory_bandwidth_gibs <= 0.0) continue;
                if (!fastest || d.memory_bandwidth_gibs > fastest->memory_bandwidth_gibs) fastest = &d;
            }
            const std::string capacity = "every device here reads the host's own memory, so moving a weight onto "
                                         "one frees nothing: the capacity tier is inert. ";
            // A tenth, not a hair: the two figures come from the same probe on the same machine, and
            // claiming "more" for a difference inside its own repeatability is how a rationale starts
            // being read as noise. Measured 19 against 19 on a phone, which is not a finding.
            if (fastest && hw.host_bandwidth_gibs > 0.0 &&
                fastest->memory_bandwidth_gibs > hw.host_bandwidth_gibs * 1.10)
                note("extra-offload", "none", Source::Measured,
                     capacity + "What is left is bandwidth, and here the device has more of it (" +
                         u64s((uint64_t) fastest->memory_bandwidth_gibs) + " against " +
                         u64s((uint64_t) hw.host_bandwidth_gibs) +
                         " GiB/s): what the fitter placed is worth having there, and the streamed experts "
                         "would be too if they could be handed over");
            else if (fastest && hw.host_bandwidth_gibs > 0.0)
                note("extra-offload", "none", Source::Measured,
                     capacity + "What is left is bandwidth, and here the device has no more of it (" +
                         u64s((uint64_t) fastest->memory_bandwidth_gibs) + " against " +
                         u64s((uint64_t) hw.host_bandwidth_gibs) + " GiB/s), so there is nothing to win");
            else
                note("extra-offload", "none", Source::Unprobed,
                     capacity + "What is left is bandwidth, and no device's own figure was measured here - "
                                "which is the number that would say whether an offload pays at all");
        } else if (candidate && candidate->memory_bandwidth_gibs <= 0.0) {
            note("extra-offload", "none", Source::Unprobed,
                 "a device with " + u64s(mib(device_local)) +
                     " MiB of its own is present and the fitter has already used it; moving anything FURTHER "
                     "would turn on its memory bandwidth, which is unmeasured here");
        } else {
            note("extra-offload", "none", Source::Policy,
                 "the fitter has already placed what fits on the devices - see the placement line above - and "
                 "placing weights across devices is a capacity problem it solves exactly; this planner owns "
                 "the tier below it and does not second-guess it");
        }

        // Where the streamed experts are COMPUTED, which is a different question from where they
        // live. They must live in a host buffer - that is absolute, since serving one means rebinding
        // its pointer onto the file's native layout, and a repack replaces exactly that layout. But a
        // device that offers a host buffer reads that same memory directly, so it can execute over
        // the bytes we read from flash without a copy and without a repack, and the pair of
        // properties is one device's answer rather than a class of hardware.
        //
        // Whether it is worth doing is a second question, and the honest gate is not "is there a
        // device" but "is compute even on the critical path". Reading an expert costs far more than
        // multiplying by it, so where the stall dominates the engine that computes is irrelevant and
        // moving the work buys nothing. Where the cache is large enough that most tokens hit, the
        // compute emerges and the faster engine is worth having. Both halves of that comparison need
        // a bandwidth figure for the device, and until one is measured this stays a stated unknown
        // rather than a decision - which on the machines where it matters most, the ones whose memory
        // is unified, is the whole of the difference between an idle accelerator and a used one.
        const ComputeDevice * host_capable = nullptr;
        for (const ComputeDevice & d : hw.devices) {
            if (d.is_cpu) continue; // the CPU is where they already are
            if (!is_yes(d.host_buffer) || d.runs_expert_op != Tri::Yes || d.needs_repack == Tri::Yes) continue;
            host_capable = &d;
            break;
        }
        const bool repack_blocks = candidate && candidate->needs_repack == Tri::Yes;
        if (host_capable && !pol.streamer_serves_device_memory)
            note("experts", "host", Source::Derived,
                 std::string("this device could compute them on paper - it offers a host buffer and runs the "
                             "model's own layout - and it still cannot. The streamer rebinds every expert onto "
                             "memory it reserved itself, which no device was given access to; and a host buffer "
                             "type would not survive the load anyway, since a mapped model has one substituted "
                             "for the CPU's. ") +
                     (is_yes(host_capable->host_ptr_buffers)
                          ? "This device can wrap memory a caller already owns, which is the one opening: the "
                            "streamer's own reservations would have to be handed over that way"
                      : host_capable->host_ptr_buffers == Tri::No
                          ? "This device cannot wrap memory a caller already owns, so there is no opening here "
                            "at all"
                          : "Whether this device can wrap memory a caller already owns - the one opening left - "
                            "is unprobed. What it advertises is a blanket promise, and a backend whose support "
                            "is conditional declines to make one while implementing the call anyway, so the "
                            "absence of a yes decides nothing here"));
        else if (host_capable && host_capable->memory_bandwidth_gibs <= 0.0)
            note("experts", "host", Source::Unprobed,
                 "this device offers a host buffer and executes this model's expert matmul on the file's own "
                 "layout, so streamed experts could be computed on it - but what that is worth is the ratio of "
                 "its bandwidth to the host's, and its own is unmeasured here");
        else if (host_capable && hw.host_bandwidth_gibs > 0.0 &&
                 host_capable->memory_bandwidth_gibs > hw.host_bandwidth_gibs)
            note("experts", "device", Source::Measured,
                 "this device offers a host buffer, executes the model's own layout, and reaches " +
                     u64s((uint64_t) host_capable->memory_bandwidth_gibs) + " GiB/s against the host's " +
                     u64s((uint64_t) hw.host_bandwidth_gibs) +
                     ": once the streamer can hand it their memory, "
                     "this is where they belong");
        else
            note("experts", "host", Source::Derived,
                 repack_blocks ? "a streamed expert must keep the file's native layout so its pointer can be "
                                 "rebound, and the device here executes only a repacked layout"
                               : "a streamed expert must live in a host buffer whose pointer we may rebind onto "
                                 "the file's native layout");
    }

    // ── the dense policy: decided by what a reclaim COSTS here, nothing else ────────
    if (!pinned("dense-weights", dense_mode_name(p.config.moe.dense_weights))) {
        if (hw.anon_overflow == Overflow::Kill && hw.file_pages_counted == Tri::No) {
            // Where the process is killed for holding too much and clean file pages are outside
            // that accounting, leaving a weight mapped costs nothing against the limit that can
            // actually end the run. This inverts the policy that is right everywhere else.
            p.config.moe.dense_weights = DenseWeightsMode::Mmap;
            note("dense-weights", "mmap", Source::Derived,
                 "holding too much is fatal here and mapped file pages are not counted against the limit, "
                 "so the dense set is cheapest left where it is");
        } else if (hw.reclaim_exempt_max > 0 && hw.anon_overflow == Overflow::Compress) {
            // Anonymous memory keeps the dense set off flash but not out of a compressed swap,
            // where every touch costs a decompression that no I/O counter shows. A store the
            // kernel may not reclaim is the only thing that closes that gap.
            p.config.moe.dense_weights = DenseWeightsMode::Pinned;
            note("dense-weights", "ahwb", Source::Derived,
                 "anonymous memory here is " + std::string(overflow_name(hw.anon_overflow)) +
                     ", and this machine offers a reclaim-exempt allocation of up to " +
                     u64s(mib(hw.reclaim_exempt_max)) + " MiB per buffer");
        } else if (hw.anon_overflow == Overflow::Unknown) {
            p.config.moe.dense_weights = DenseWeightsMode::Anonymous;
            note("dense-weights", "anon", Source::Policy,
                 "what a reclaim costs here was not probed; anonymous buffers are the default that at "
                 "least keeps the dense set off a flash refault");
        } else {
            p.config.moe.dense_weights = DenseWeightsMode::Anonymous;
            note("dense-weights", "anon", Source::Derived,
                 "no reclaim-exempt store here, so anonymous buffers are what keeps the dense set off a "
                 "flash refault (a reclaim then means: " +
                     std::string(overflow_name(hw.anon_overflow)) + ")");
        }
    }

    // Bytes the dense policy is about to take out of reclaimable page cache. Mirrors the runtime's
    // own rule: a tensor bigger than what this process may hold is never converted, it stays mapped.
    // The comparison stays on the REPORTED figure rather than the measured headroom, because it is
    // mirroring a decision the runtime makes from the reported one - a plan that predicted a
    // conversion the engine will then refuse would be worse than one that is merely conservative.
    const bool converts_dense = p.config.moe.dense_weights == DenseWeightsMode::Anonymous ||
                                p.config.moe.dense_weights == DenseWeightsMode::Pinned;
    uint64_t dense_pending = converts_dense ? host_dense : 0;
    if (converts_dense && model.largest_dense_tensor > hw.residency_budget)
        dense_pending = dense_pending > model.largest_dense_tensor ? dense_pending - model.largest_dense_tensor : 0;
    p.dense_pending_bytes = dense_pending;

    // ── the cache budget, and the floor that is the model's own arithmetic ──────────
    const uint64_t cycle = model.token_cycle_bytes;
    p.token_cycle_bytes = cycle;

    const uint64_t for_cache_raw = usable > dense_pending ? usable - dense_pending : 0;
    const uint64_t budget = std::min(for_cache_raw, host_expert_bytes);
    p.cache_budget_bytes = budget;

    if (cycle == 0) {
        decline("the model's expert shapes could not be read, so the cache floor could not be derived");
        note("moe-stream", "off", Source::Unprobed, "expert slice size unknown");
        return p;
    }
    if (budget < cycle && req.is_pinned("cache-mb")) {
        note("cache-floor", u64s(mib(cycle)) + " MiB", Source::Derived,
             "the derived budget (" + u64s(mib(budget)) +
                 " MiB) is under this model's token cycle, but the caller "
                 "pinned cache-mb and a pin is the caller's authority: proceeding with the pinned value");
    } else if (budget < cycle) {
        decline("a cache of " + u64s(mib(budget)) + " MiB is below this model's worst-case token cycle of " +
                u64s(mib(cycle)) +
                " MiB: under that floor the cache evicts what the same token still needs, "
                "so it costs its memory and returns no hits at all");
        note("moe-stream", "off", Source::Derived, "budget below the derived cache floor");
        return p;
    }

    p.config.moe.enabled = true;
    note("moe-stream", "on", Source::Derived,
         p.regime == Regime::ExpertsStream
             ? "the experts do not fit but the dense set does"
             : "neither set fits, so the experts stream and the dense policy degrades with them "
               "(a tensor larger than this budget is left mapped rather than made resident)");

    if (!pinned("cache-mb", u64s((uint64_t) std::max(0, p.config.moe.cache_mb)))) {
        p.config.moe.cache_auto = false;
        p.config.moe.cache_mb = (int) mib(budget);
        note("cache-mb", u64s(mib(budget)), Source::Derived,
             u64s(mib(usable)) + " MiB usable less " + u64s(mib(dense_pending)) +
                 " MiB the dense policy is about "
                 "to make non-reclaimable, capped at the " +
                 u64s(mib(model.expert_bytes)) + " MiB the experts occupy");

        // The generic 1500 MiB guard predates the per-model floor and is the weaker of the two: a
        // budget above this model's own token cycle is not pathological however small it looks.
        if (p.config.moe.cache_mb > 0 && p.config.moe.cache_mb < MoeStreamConfig::cache_min_mb) {
            p.config.moe.force_cache = true;
            note("force-cache", "on", Source::Derived,
                 "the budget clears this model's token cycle of " + u64s(mib(cycle)) +
                     " MiB but sits under the generic " + u64s(MoeStreamConfig::cache_min_mb) +
                     " MiB guard, so the derived floor is used as the authority");
        }
    }

    // ── the KV, as a budget line rather than a silent subtraction ───────────────────
    // Context memory competes with the expert cache for the same bytes, and until now it only
    // appeared inside the placement's rationale. On a long context it is the larger of the two, and
    // a reader deciding whether to shorten the prompt deserves to see the trade rather than infer
    // it. Nothing is DECIDED here: llama.cpp's own defaults already pick flash attention when the
    // build supports it, which costs no quality, and the one lever that would free real memory -
    // quantizing the K and V caches - changes the output. That makes it lossy by this planner's
    // definition, and lossy levers are the caller's to arm, never a plan's.
    if (placement.fitted && placement.raw_host_context_bytes > 0) {
        const uint64_t kv = placement.raw_host_context_bytes;
        const uint64_t compute = placement.raw_host_compute_bytes;
        note("kv-budget", u64s(mib(kv)) + " MiB", Source::Measured,
             "the context reserves " + u64s(mib(kv)) + " MiB on the host and its compute buffers another " +
                 u64s(mib(compute)) + ", against " + u64s(mib(budget)) +
                 " MiB left for the expert cache: the two come out of the same memory, and a shorter context "
                 "is the caller's cheapest way to buy cache. Quantizing the K/V caches would buy more and "
                 "changes the output, so it stays a lever this plan will not pull");
    }

    // ── the I/O policy: from the machine's own rate curve, never from a class name ──
    if (!pinned("io-threads", u64s((uint64_t) p.config.moe.io_threads))) {
        const uint32_t want = hw.storage.best_lanes((uint32_t) model.expert_slice_bytes);
        if (want > 0) {
            const int lanes = std::min<int>((int) want, MoeStreamConfig::io_threads_max);
            p.config.moe.io_threads = std::max(1, lanes);
            note("io-threads", u64s((uint64_t) p.config.moe.io_threads), Source::Measured,
                 "the highest measured rate for this model's " + u64s(model.expert_slice_bytes >> 10) +
                     " KiB expert slice on this storage");
        } else {
            note("io-threads", u64s((uint64_t) p.config.moe.io_threads), Source::Unprobed,
                 "no read-rate curve for this machine; keeping the default lane count");
        }
    }

    if (!pinned("o-direct", p.config.moe.o_direct ? "on" : "off")) {
        if (hw.storage.direct_ok == Tri::Yes) {
            p.config.moe.o_direct = true;
            note("o-direct", "on", Source::Measured, "uncached reads return correct bytes on this path");
        } else if (hw.storage.direct_ok == Tri::No) {
            p.config.moe.o_direct = false;
            note("o-direct", "off", Source::Measured,
                 "uncached reads do not return correct bytes on this path, so they are not used");
        } else {
            note("o-direct", p.config.moe.o_direct ? "on" : "off", Source::Unprobed,
                 "unverified here; the reader checks at open and falls back to buffered reads by itself");
        }
    }

    if (!pinned("release-mmap", p.config.moe.release_mmap ? "on" : "off")) {
        // Carrying a multi-token-prediction block is not the same as using one: llama.cpp does not
        // load that block unless the run asks for it, and an unloaded block holds no pointer into
        // the mapping. Testing the file instead of the run would silently disable this on every
        // model that merely ships an MTP head, which is most of them.
        const bool mtp_in_use = model.has_mtp && p.config.spec.is_mtp();
        const bool safe_shape =
            !model.tied_output_head && !mtp_in_use && p.config.moe.dense_weights != DenseWeightsMode::Mmap;
        // Safety is reported before the probe, because it is the stronger constraint: on a shape
        // that keeps a live pointer into the mapping, measuring the storage would change nothing.
        if (!safe_shape) {
            note("release-mmap", "off", Source::Derived,
                 "this run keeps a live pointer into the mapping (a tied head, a draft block, or a dense "
                 "policy that stays mapped), so the mapping cannot be released whatever the storage does");
        } else if (hw.storage.mapping_serialises_reads == Tri::Yes) {
            p.config.moe.release_mmap = true;
            note("release-mmap", "on", Source::Measured,
                 "a live mapping of the model serialises this storage's concurrent uncached reads (" +
                     u64s((uint64_t) hw.storage.rate_mapped_mibs) + " vs " +
                     u64s((uint64_t) hw.storage.rate_unmapped_mibs) +
                     " MiB/s), and this model's shape keeps no pointer that would survive the release "
                     "(the session checks again before doing it)");
        } else {
            note("release-mmap", p.config.moe.release_mmap ? "on" : "off", Source::Unprobed,
                 hw.storage.rate_unmapped_mibs > 0.0
                     ? "the storage probe did not see a live mapping slow its reads here (" +
                           u64s((uint64_t) hw.storage.rate_mapped_mibs) + " vs " +
                           u64s((uint64_t) hw.storage.rate_unmapped_mibs) +
                           " MiB/s), but it is an instrument validated only in the positive direction, so "
                           "that is not evidence there is nothing to gain: try --release-mmap and measure"
                     : "whether a live mapping serialises reads was not measured on this machine");
        }
    }

    // ── everything lossy: the caller's authority, never ours ────────────────────────
    const bool lossy_pinned =
        req.is_pinned("drop-cold-experts") || req.is_pinned("expert-substitute") || req.is_pinned("route-ahead");
    if (lossy_pinned) {
        note("lossy", "as set", Source::Operator, "the caller armed a lossy policy explicitly");
    } else if (req.quality_budget <= 0.0f) {
        p.config.moe.drop_cold_frac = 0.0f;
        p.config.moe.substitute_lambda = 0.0f;
        p.config.moe.route_ahead = 0;
        note("lossy", "off", Source::Policy,
             "no quality budget was given, and a policy that changes the output must never arm itself");
    } else {
        p.config.moe.drop_cold_frac = 0.0f;
        p.config.moe.substitute_lambda = 0.0f;
        p.config.moe.route_ahead = 0;
        note("lossy", "off", Source::Unprobed,
             "a quality budget was given, but no measured mapping from a perplexity cost to a lever value "
             "exists yet, so spending it would be a guess");
    }

    // A placement the fitter made and this planner did not apply, because on shared memory its
    // capacity arithmetic counts the same pool twice. Said out loud: "no layers on devices" on a
    // machine that has one is a sentence a reader would otherwise take as "there is no device".
    // Nothing is going on a device, so no device should be in the graph. It is not enough to decline
    // a placement: a registered backend collects work on its own, and every piece it collects is a
    // crossing. Measured with zero layers placed - 61 splits a token, and a build carrying the
    // backend losing to one without it. A caller who wants the device anyway pins the knob.
    if (!req.is_pinned("devices") && p.config.n_gpu_layers == 0 && !p.config.dense_on_device) {
        size_t non_cpu = 0;
        for (const ComputeDevice & d : hw.devices)
            if (!d.is_cpu) ++non_cpu;
        if (non_cpu > 0) {
            p.config.devices_cpu_only = true;
            note("devices", "cpu only", Source::Derived,
                 u64s(non_cpu) +
                     " device(s) registered and no weight going to any of them: a backend left in the graph "
                     "takes the nodes it can execute simply for being there, and each is a boundary crossed "
                     "twice. Measured at 61 splits a token with nothing placed");
        }
    }

    if (device_on_host > 0)
        note("shared-pool", u64s(mib(device_on_host)) + " MiB", Source::Derived,
             "every device here reads this host's own memory, so what the fitter placed on one is in the "
             "same pool as what it left here and is charged to it: " +
                 u64s(mib(device_on_host)) + " MiB added to the " + u64s(mib(placement.host_resident_bytes)) +
                 " MiB host set before anything was sized. The fitter treats the two as separate, which is "
                 "how it came to report 15 GB free on an 11 GB machine");

    if (placement.shared_memory_placement)
        note("placement-declined", u64s(mib(placement.device_bytes)) + " MiB", Source::Derived,
             "the capacity fitter would have placed " + u64s(mib(placement.device_bytes)) +
                 " MiB on a device whose memory is this host's own, counting it as separate from the " +
                 u64s(mib(placement.host_resident_bytes)) +
                 " MiB it left here - the same pool, twice. Acting on that took a phone down; charging it "
                 "instead reads as a model twice its real size and declines a run that works. The number is "
                 "not wrong by an amount, it is about a distinction this machine does not have, so the "
                 "placement is quoted and not applied. Using such a device is a bandwidth decision, not a "
                 "capacity one");

    // ── the dense set on a device that shares this memory ──────────────────────────
    // The capacity tier said nothing here, so this is the bandwidth decision it left behind. The
    // dense set is small - a fraction of the file - and the device reads this model's weights
    // faster than the cores do, so computing it there is the whole of what an integrated
    // accelerator can offer. It costs no capacity, because the memory is the same memory.
    //
    // Armed by the caller, not by this plan. What is missing is the device's own compute buffer as
    // a budget line: on one phone it reserved 906 MiB against the CPU's 22, and a plan that sizes a
    // cache without knowing that is a plan that takes the machine down - which it did, once.
    if (devices_share_host_memory && !p.config.dense_on_device) {
        const ComputeDevice * fast = nullptr;
        for (const ComputeDevice & d : hw.devices) {
            if (d.is_cpu || d.runs_expert_op != Tri::Yes || d.needs_repack == Tri::Yes) continue;
            if (hw.host_bandwidth_gibs <= 0.0 || d.memory_bandwidth_gibs <= hw.host_bandwidth_gibs * 1.10) continue;
            fast = &d;
            break;
        }
        if (fast)
            note("dense-on-device", "off", Source::Unprobed,
                 "this device reads the model's own weights at " + u64s((uint64_t) fast->memory_bandwidth_gibs) +
                     " GiB/s against the host's " + u64s((uint64_t) hw.host_bandwidth_gibs) +
                     ", and the dense set is " + u64s(mib(model.dense_bytes)) +
                     " MiB - small enough that computing it there costs no memory on a shared pool. Arm it with "
                     "--dense-on-device; this plan will not, until the device's compute buffer is a budget line");
    }
    if (p.config.dense_on_device && devices_share_host_memory) {
        // Refused, and the refusal is the finding. Placing the dense set on a shared-memory device
        // and routing the experts back with an `exps` override loads, reserves, starts the streamer
        // - and then segmentation-faults, three times out of three on the phone it was tried on,
        // while the same run without it is fine. Standing the dense policy down (it would rebind a
        // pointer it does not own) moved the fault later rather than removing it, so something else
        // in the streamed path is still handed a device address. Until that is found this arms
        // nothing: a lever that takes the machine down is worse than one that does not exist.
        p.config.dense_on_device = false;
        note("dense-on-device", "refused", Source::Derived,
             "refused, and twice over. This path faults once the expert streamer starts, reproducibly - the "
             "expert override here is a literal pattern where the earlier `--gpu` built one from the "
             "architecture recipe, which is a bug and a small one. The larger reason is that the same "
             "placement was measured three times on a device of this class and LOST: full offload -27% on a "
             "Q4_0 run, because the dense and expert halves interleave, so a two-device split crosses the "
             "boundary twice per layer - 96 splits against 1 - and the boundary tax eats the CPU time the "
             "device frees. The untested shape that could change it is contiguous layer BLOCKS, experts "
             "included and resident, which puts the boundary in one place");
    }
    // ── overlap: named, not armed ──────────────────────────────────────────────────
    // Hiding compute behind the reads is not a quality choice - the output is byte-identical - and
    // it is worth about 5% on the phone this was measured on, where a plan without it lost to a
    // hand-written recipe that had it. It is still not armed, and the reason is the same one that
    // withdrew the thread rule two blocks down: one machine's evidence is not a rule. What the plan
    // can do honestly is say the knob exists and that the caller is the one who knows.
    if (!req.is_pinned("overlap") && !p.config.moe.overlap)
        note("overlap", "off", Source::Unprobed,
             "hiding compute behind the reads changes no output and measured +5% on one phone, but one "
             "machine is not a rule and nothing here predicts where it stops paying: pass --overlap");

    // ── threads: the classes are a fact, the rule that read them was wrong ─────────
    // The rule here used to set the count to the fast class, reasoning that every thread meets the
    // same barrier so a thread on a slower core sets the pace rather than adding to it. Sound, and
    // refuted by the first heterogeneous machine it met: a phone with two prime cores and six others
    // got two threads instead of four, and decode compute went from 0.127 to 0.195 s/token - 4.24
    // tok/s down to 2.68. Losing half the threads costs more than the barrier's imbalance does,
    // and nothing here knows where that trade turns over.
    //
    // So the classes stay as a measured fact, printed because they are worth knowing, and the knob
    // keeps its default. Deriving a count from them would need a thread sweep on the machine itself,
    // which is a probe this does not have. The same applies to prefill: it is compute-bound and
    // plausibly wants every core, and "plausibly" is exactly what this planner does not ship.
    if (!req.is_pinned("threads")) {
        if (hw.best_threads > 0) {
            p.config.n_threads = (int) hw.best_threads;
            note("threads", u64s(hw.best_threads), Source::Measured,
                 "swept over the thread counts this machine has, on a matmul in this model's own "
                 "quantized format - which is the material that matters: measured in F32 instead, the same "
                 "sweep answered 2 on a phone whose engine is 58% faster at 4");
        } else if (hw.core_classes.size() > 1) {
            std::string shape;
            for (size_t k = 0; k < hw.core_classes.size(); ++k)
                shape += (k ? " + " : "") + u64s(hw.core_classes[k]);
            note("threads", u64s((uint64_t) p.config.n_threads), Source::Unprobed,
                 "this machine's cores are not alike (" + shape +
                     " by reported maximum frequency), which is measured - but the rule that turned that "
                     "into a count was refuted on a machine of exactly this shape, so the default stands "
                     "until a thread sweep says otherwise");
        } else if (hw.core_classes.size() == 1) {
            note("threads", u64s((uint64_t) p.config.n_threads), Source::Derived,
                 "every core here is alike, so there is no slower class to spill onto and the count is not "
                 "the planner's to improve");
        } else {
            note("threads", u64s((uint64_t) p.config.n_threads), Source::Unprobed,
                 "this machine does not report per-core maximum frequencies, so its core classes are unknown");
        }
    }

    if (!req.is_pinned("ubatch"))
        note("ubatch", u64s((uint64_t) p.config.n_ubatch), Source::Unprobed,
             "the compute-buffer reservation trades against the cache, but the crossover is unmeasured on "
             "this machine; keeping the default");

    return p;
}

} // namespace bmoe
