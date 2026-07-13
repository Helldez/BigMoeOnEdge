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
    };
    virtual Stats stats() const = 0;
};

} // namespace bmoe
