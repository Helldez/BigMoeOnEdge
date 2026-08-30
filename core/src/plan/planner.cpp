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
    const uint64_t by_frac = (uint64_t) ((double) hw.residency_budget * frac);
    return std::max(by_frac, pol.margin_min_bytes);
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
    const uint32_t n_layer = model.n_layer;
    const uint32_t host_layers = placement.fitted ? (uint32_t) placement.host_expert_layers.size() : n_layer;
    const uint64_t host_expert_bytes = (placement.fitted && n_layer)
                                           ? (uint64_t) ((double) model.expert_bytes * host_layers / n_layer)
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
    const uint64_t usable = hw.residency_budget > margin ? hw.residency_budget - margin : 0;

    // The bytes that would have to be resident on the host if nothing were streamed: with a
    // placement, what the fitter left here; without one, the whole file.
    const uint64_t host_all = placement.fitted ? placement.host_resident_bytes + host_expert_bytes : model.file_bytes;
    const uint64_t host_dense = placement.fitted ? placement.host_resident_bytes : model.dense_bytes;
    if (host_all <= usable) {
        p.regime = Regime::Fits;
    } else if (host_dense <= usable) {
        p.regime = Regime::ExpertsStream;
    } else {
        p.regime = Regime::DenseOversized;
    }

    if (placement.fitted) {
        note("placement", u64s((uint64_t) std::max(0, placement.n_gpu_layers)) + " layers on devices", Source::Measured,
             placement.outcome + "; " + u64s(host_layers) + " of " + u64s(n_layer) +
                 " layers keep their experts on the host, host-resident " + u64s(mib(placement.host_resident_bytes)) +
                 " MiB, on devices " + u64s(mib(placement.device_bytes)) + " MiB, context " + u64s(placement.n_ctx) +
                 " (fitter's host breakdown: model " + u64s(mib(placement.raw_host_model_bytes)) + ", context " +
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
        note("placement", "none", Source::Unprobed,
             "the capacity fitter was not run, so no layer is placed on a device: everything is on the host");
    }

    note("regime", regime_name(p.regime), Source::Derived,
         "on the host " + u64s(mib(host_all)) + " MiB (dense " + u64s(mib(host_dense)) + ") against " +
             u64s(mib(usable)) + " MiB usable, which is the " + u64s(mib(hw.residency_budget)) +
             " MiB this process may hold less a " + u64s(mib(margin)) + " MiB margin (" +
             overflow_name(hw.anon_overflow) + ")");

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

    // ── the accelerator axis: what may leave the host, and what may never ──────────
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
            note("offload", "host", Source::Unprobed,
                 "no compute devices were enumerated (the backends register at load), so nothing could "
                 "be considered for offload");
        } else if (device_local == 0) {
            note("offload", "host", Source::Derived,
                 "every device here reads the host's own memory, so moving a weight onto one frees no "
                 "memory and wins no bandwidth: batch-1 decode is bound by bandwidth, not by arithmetic");
        } else if (candidate && candidate->memory_bandwidth_gibs <= 0.0) {
            note("offload", "host", Source::Unprobed,
                 "a device with " + u64s(mib(device_local)) +
                     " MiB of its own is present, but its memory bandwidth is unmeasured here and that is "
                     "the number a decode offload turns on");
        } else {
            note("offload", "host", Source::Policy,
                 "placing weights across devices is a capacity problem that llama.cpp's own fitter "
                 "solves; this planner owns the tier below it and does not duplicate it");
        }

        // Stated whatever the outcome above, because it is the condition this engine exists under.
        const bool repack_blocks = candidate && candidate->needs_repack == Tri::Yes;
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

    // ── knobs with no rule yet: named, so the missing measurement stays visible ─────
    if (!req.is_pinned("threads"))
        note("threads", u64s((uint64_t) p.config.n_threads), Source::Unprobed,
             "no measured rule relates core topology to decode throughput here; keeping the default");
    if (!req.is_pinned("ubatch"))
        note("ubatch", u64s((uint64_t) p.config.n_ubatch), Source::Unprobed,
             "the compute-buffer reservation trades against the cache, but the crossover is unmeasured on "
             "this machine; keeping the default");

    return p;
}

} // namespace bmoe
