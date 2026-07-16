// The expert-residency port.
//
// An IExpertSource owns where a MoE layer's expert weights live in memory and makes the
// routed experts of a given layer resident just before that layer's expert matmul runs.
// The engine's router hook calls load_layer() with the expert ids the graph selected;
// the implementation blocks until those experts are in place.
//
// This is the seam that keeps the streaming strategy swappable: the default adapter
// (ExpertStreamSource) reads slices from flash on demand with an optional LRU cache, but
// a different residency policy (all-resident, network-fetched, ...) is just another
// implementation of this interface.
#pragma once

#include <cstdint>

namespace bmoe {

class IExpertSource {
public:
    virtual ~IExpertSource() = default;

    // Make layer `il`'s routed experts resident. `ids` holds n_ids expert indices
    // (duplicates allowed; the union across a batch's tokens for prefill, exactly the
    // top-k for n=1 decode). In the default (serial) mode this BLOCKS until every routed
    // slice is in place at its canonical offset inside the bound tensor. In overlap mode
    // it only publishes the reads and returns immediately; the layer's matmul then blocks
    // per expert (via the fork's expert-ready hook) until that expert's slice arrives.
    // Returns false on I/O failure (serial) or if a prior async batch already failed.
    virtual bool load_layer(int il, const int32_t * ids, int n_ids) = 0;

    // Hint that layer `il` is likely to route `ids` (n_ids of them) on a future token, so the
    // implementation may read them ahead on otherwise-idle lanes. Purely advisory: a correct
    // guess makes the later load_layer(il, …) a cache hit, a wrong guess wastes a read — neither
    // changes what load_layer produces. Default: no-op (a source without a speculative path).
    virtual void prefetch(int /*il*/, const int32_t * /*ids*/, int /*n_ids*/) {}

    // ── route-trace support (diagnostics only; see bmoe/route_trace.h) ──────────────────
    // These let a tracer describe what a routing COST without changing what it does. All three
    // are eval-thread only, and meaningful only between the routing node and load_layer().

    // Integrate any speculative prefetch that has landed, so a residency query taken right
    // after sees the true cache state. load_layer() already does this itself; a tracer must ask
    // for it explicitly BEFORE querying, or an expert a prefetch correctly guessed still reads
    // as a miss. Default: a source that never speculates.
    virtual void settle_spec() {}

    // Classify layer `il`'s `ids` against the cache as it stands NOW, writing one
    // RouteResidency per id into `out`. Call before load_layer(il, ...) makes them resident.
    // Default: a source with no cache, where every routing reads.
    virtual void query_residency(int /*il*/, const int32_t * /*ids*/, int n_ids, uint8_t * out) const {
        for (int i = 0; i < n_ids; ++i)
            out[i] = 0;
    }

    // Flash bytes one expert of layer `il` occupies across its projections — what a miss costs.
    virtual uint64_t expert_bytes(int /*il*/) const { return 0; }

    // Cumulative streaming statistics, for telemetry and the end-of-run summary.
    struct Stats {
        uint64_t read_bytes = 0;           // bytes pulled from flash (aligned windows)
        double read_seconds = 0.0;         // wall time spent in the read phase
        double mgmt_seconds = 0.0;         // cache management: vm commit + evict + LRU bookkeeping
        long long cache_hits = 0;          // expert lookups served from the cache
        long long cache_lookups = 0;       // total expert lookups (hits + misses)
        uint64_t cache_resident_bytes = 0; // currently resident cached slice bytes
        double stall_seconds = 0.0;        // overlap: summed across compute threads (0 when serial)
        uint64_t spec_read_bytes = 0;      // bytes read speculatively by prefetch (subset of read_bytes)
        long long spec_experts = 0;        // experts fully prefetched
        long long spec_useful = 0;         // prefetched experts that a later lookup actually hit
        uint64_t cache_budget_bytes = 0;   // current cache budget (moves under --cache-mb auto)
        long long cache_resizes = 0;       // times the budget changed at runtime (auto + explicit)

        // ── pressure sensing (--cache-dynamic; see bmoe/cache_governor.h) ──
        // Sampled fraction of the cache's own pages still in RAM, or -1 when not measured (sampler
        // throttled, sensing off, or a platform that cannot report). Below 1 means the kernel is
        // reclaiming the cache out from under us.
        double cache_resident_frac = -1.0;
        // Sampled fraction of the DENSE weights (the mmap'd model) still in RAM, or -1 when not
        // measured. The companion to cache_resident_frac: that one watches our anon cache, this one
        // the file-backed weights it is blind to. Dense falling while cache holds means the faults
        // are the model, not the cache — and shrinking the cache cannot help.
        double dense_resident_frac = -1.0;
        // Bytes of distinct experts one token routes, measured (0 = not yet known). What a cache
        // must clear to hold anything BETWEEN tokens — where hits start, not a floor to defend:
        // on a >RAM model it can exceed what the device concedes. See bmoe/cache_governor.h.
        uint64_t token_demand_bytes = 0;
        // Bytes the widest single layer routes, measured (0 = not yet known). The mechanical floor:
        // the cache must hold the layer being staged.
        uint64_t layer_demand_bytes = 0;
    };
    virtual Stats stats() const = 0;
};

} // namespace bmoe
