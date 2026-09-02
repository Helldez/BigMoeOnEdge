// Value-type helpers for the planning inputs and output: rate-curve lookup, device accounting, and
// the rendered rationale. No rules live here — those are in planner.cpp.

#include "bmoe/hardware_profile.h"
#include "bmoe/model_profile.h"
#include "bmoe/plan.h"

#include <algorithm>
#include <cstdio>

namespace bmoe {

double StorageFacts::rate_at(uint32_t request_bytes, uint32_t lanes) const {
    // Nearest sample rather than an interpolation. The curve's shape is dominated by a per-request
    // latency floor, so a value invented between two samples would claim throughput at a request
    // size nobody measured — exactly the guess this whole design exists to avoid.
    const RateSample * best = nullptr;
    uint64_t best_dist = 0;
    for (const RateSample & s : rate_curve) {
        if (s.lanes != lanes) continue;
        const uint64_t d =
            s.request_bytes > request_bytes ? s.request_bytes - request_bytes : request_bytes - s.request_bytes;
        if (!best || d < best_dist) {
            best = &s;
            best_dist = d;
        }
    }
    return best ? best->mib_per_s : 0.0;
}

uint32_t StorageFacts::best_lanes(uint32_t request_bytes) const {
    double best_rate = 0.0;
    for (const RateSample & s : rate_curve)
        best_rate = std::max(best_rate, rate_at(request_bytes, s.lanes));
    if (best_rate <= 0.0) return 0;

    // The LARGEST lane count that reaches within a few percent of the best rate.
    //
    // This used to be the smallest, on the reasoning that where two lane counts deliver the same
    // throughput the cheaper one is strictly better - fewer threads, less queueing, less contention
    // with the compute the reads are supposed to be overlapping. That reasoning is sound and the
    // measurement refutes it. On one desktop SSD the probe cannot separate two lanes from four: it
    // picks either across repeated runs, medians and all. The engine on that same machine is not
    // ambiguous at all - three interleaved 64-token runs give 4.648 tok/s at two lanes against
    // 5.261 at four, +13%, with no overlap between the groups.
    //
    // So a lane buys something this probe does not measure. Aggregate throughput is what it reads;
    // what a streamed decode also spends is LATENCY, waiting for the slice that the next expert
    // needs, and a queue that empties sooner ends the stall sooner even when the bytes per second
    // come out the same. Until that is measured directly, the tie-break follows the evidence rather
    // than the principle: inside the probe's own resolution, more lanes.
    const double good_enough = best_rate * 0.95;
    uint32_t pick = 0;
    for (const RateSample & s : rate_curve) {
        if (rate_at(request_bytes, s.lanes) < good_enough) continue;
        if (s.lanes > pick) pick = s.lanes;
    }
    return pick;
}

const char * group_name(WeightGroup g) {
    switch (g) {
    case WeightGroup::Embedding:
        return "embedding";
    case WeightGroup::Attention:
        return "attention";
    case WeightGroup::DenseFfn:
        return "dense-ffn";
    case WeightGroup::Experts:
        return "experts";
    case WeightGroup::Output:
        return "output";
    case WeightGroup::Other:
        return "other";
    default:
        return "?";
    }
}

uint64_t ModelProfile::bytes_per_token() const {
    uint64_t total = 0;
    for (int i = 0; i < (int) WeightGroup::count; ++i)
        total += groups[i].bytes_per_token;
    return total;
}

uint64_t HardwareProfile::device_local_memory() const {
    uint64_t total = 0;
    for (const ComputeDevice & d : devices)
        if (!d.host_memory) total += d.memory_total;
    return total;
}

std::string Plan::explain() const {
    std::string out;
    out += "plan: regime ";
    out += regime_name(regime);
    out += "\n";

    if (token_cycle_bytes)
        out += "plan:   token cycle " + std::to_string((unsigned long long) (token_cycle_bytes >> 20)) + " MiB\n";

    for (const Decision & d : decisions) {
        // Fixed-width source tag so a reader can scan the confidence column, which is the column
        // that matters: a page of "unprobed" is a page of measurements the project still owes.
        char head[96];
        std::snprintf(head, sizeof(head), "plan:   %-14s %-10s [%s] ", d.knob.c_str(), d.value.c_str(),
                      source_name(d.source));
        out += head;
        out += d.reason;
        out += "\n";
    }

    // The allocation, last, because it is the part a reader checks the prediction against: what
    // each group got, what it is predicted to cost, and what the same token would have cost with
    // nothing resident. The bound is printed as a bound and never divided into the prediction to
    // manufacture a speedup figure - the page cache retains some of a mapped model and readahead
    // amortises some of the rest, both by an amount no plan can know.
    if (allocation.seconds_per_token > 0.0) {
        char head[160];
        std::snprintf(head, sizeof(head), "plan:   cost %.1f ms/token predicted, %.1f ms/token with nothing resident\n",
                      allocation.seconds_per_token * 1000.0, allocation.seconds_worst_case * 1000.0);
        out += head;
        for (int i = 0; i < (int) WeightGroup::count; ++i) {
            const GroupPlacement & g = allocation.groups[i];
            if (g.seconds_per_token <= 0.0 && g.resident_bytes == 0) continue;
            char line[256];
            std::snprintf(line, sizeof(line), "plan:     %-10s %-14s %6llu MiB held, %7.2f ms/token  ",
                          group_name(g.group), lane_name(g.lane), (unsigned long long) (g.resident_bytes >> 20),
                          g.seconds_per_token * 1000.0);
            out += line;
            out += g.reason;
            out += "\n";
        }
    }

    if (streaming_declined) {
        out += "plan:   declined: ";
        out += decline_reason;
        out += "\n";
    }
    return out;
}

} // namespace bmoe
