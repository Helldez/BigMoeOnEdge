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
    return plan_run(base, hw, model, req, PlannerPolicy::defaults());
}

Plan plan_run(const RunConfig & base,
              const HardwareProfile & hw,
              const ModelProfile & model,
              const PlanRequest & req,
              const PlannerPolicy & pol) {
    Plan p;
    p.config = base;

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

    if (model.file_bytes <= usable) {
        p.regime = Regime::Fits;
    } else if (model.dense_bytes <= usable) {
        p.regime = Regime::ExpertsStream;
    } else {
        p.regime = Regime::DenseOversized;
    }

    note("regime", regime_name(p.regime), Source::Derived,
         "model " + u64s(mib(model.file_bytes)) + " MiB (dense " + u64s(mib(model.dense_bytes)) + ") against " +
             u64s(mib(usable)) + " MiB usable, which is the " + u64s(mib(hw.residency_budget)) +
             " MiB this process may hold less a " + u64s(mib(margin)) + " MiB margin (" +
             overflow_name(hw.anon_overflow) + ")");

    if (p.regime == Regime::Fits) {
        decline("the whole model fits in this machine's residency budget; streaming it from flash "
                "would only add reads that resident weights do not need");
        note("moe-stream", "off", Source::Derived, "the model fits: hand the placement to llama.cpp instead");
        return p;
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
    uint64_t dense_pending = converts_dense ? model.dense_bytes : 0;
    if (converts_dense && model.largest_dense_tensor > hw.residency_budget)
        dense_pending = dense_pending > model.largest_dense_tensor ? dense_pending - model.largest_dense_tensor : 0;
    p.dense_pending_bytes = dense_pending;

    // ── the cache budget, and the floor that is the model's own arithmetic ──────────
    const uint64_t cycle = model.token_cycle_bytes;
    p.token_cycle_bytes = cycle;

    const uint64_t for_cache_raw = usable > dense_pending ? usable - dense_pending : 0;
    const uint64_t budget = std::min(for_cache_raw, model.expert_bytes);
    p.cache_budget_bytes = budget;

    if (cycle == 0) {
        decline("the model's expert shapes could not be read, so the cache floor could not be derived");
        note("moe-stream", "off", Source::Unprobed, "expert slice size unknown");
        return p;
    }
    if (budget < cycle) {
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

    if (!pinned("cache-mb", u64s(mib(p.config.moe.cache_mb)))) {
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
                 "a live mapping of the model serialises this storage's concurrent uncached reads, and this "
                 "model's shape has no pointer that would survive the release (the session checks again)");
        } else {
            note("release-mmap", p.config.moe.release_mmap ? "on" : "off", Source::Unprobed,
                 "whether a live mapping serialises reads was not measured on this machine");
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
