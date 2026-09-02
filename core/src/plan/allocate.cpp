// The cost model and the allocation. Read bmoe/allocate.h first: it states the unit everything is
// priced in and the three rules this file exists to obey.
//
// The shape of the algorithm is deliberately small enough to argue with:
//
//   1. Price each group in each LEGAL lane. A lane the engine cannot execute for that group, or one
//      whose deciding measurement is missing, is not cheap — it is absent.
//   2. Rank the candidates by SECONDS SAVED PER BYTE OF RAM and spend the budget down that list.
//      The expert cache is one more candidate on the same list — a fractional one, above its own
//      floor — rather than a special case that receives whatever is left over by accident.
//   3. Offer each RESIDENT group to a device, subject to the margin AND to the price of the graph
//      crossings the move creates. On a streamed group the read dominates the compute by more than
//      an order of magnitude, so a faster engine would hide behind the same I/O and buy nothing.

#include "bmoe/allocate.h"

#include "bmoe/planner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace bmoe {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kMiB = 1024.0 * 1024.0;

std::string mibs(uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu MiB", (unsigned long long) (bytes >> 20));
    return buf;
}

// Seconds to move `bytes` at `mib_per_s`. An unmeasured rate is infinite rather than zero: a
// candidate priced from a missing fact must never look cheap.
double seconds_at_mibs(uint64_t bytes, double mib_per_s) {
    return mib_per_s > 0.0 ? (double) bytes / kMiB / mib_per_s : kInf;
}

double seconds_at_gibs(uint64_t bytes, double gib_per_s) {
    return gib_per_s > 0.0 ? (double) bytes / (kMiB * 1024.0) / gib_per_s : kInf;
}

double finite_or_zero(double v) {
    return std::isfinite(v) ? v : 0.0;
}

} // namespace

const char * lane_name(Lane l) {
    switch (l) {
    case Lane::Mmap:
        return "mmap";
    case Lane::MmapWarm:
        return "mmap-warm";
    case Lane::Resident:
        return "resident";
    case Lane::Pinned:
        return "pinned";
    case Lane::RowStream:
        return "row-stream";
    case Lane::ExpertStream:
        return "expert-stream";
    default:
        return "?";
    }
}

Allocation allocate(const HardwareProfile & hw,
                    const ModelProfile & model,
                    const AllocationInputs & in,
                    const PlannerPolicy & pol) {
    Allocation a;
    a.budget_bytes = in.budget_bytes;

    for (int i = 0; i < (int) WeightGroup::count; ++i) {
        a.groups[i].group = (WeightGroup) i;
        a.groups[i].lane = Lane::Mmap;
    }

    // ── the rates every price is built from ──────────────────────────────────────────────────
    // Compute: how fast the host consumes this model's own weight bytes. Not a raw bandwidth
    // figure, and the difference is the point — a quantized matmul carries dequantisation per byte.
    const double host_gibs = hw.host_bandwidth_gibs;

    // The refault price. What a mapped group costs once pressure has dropped its pages, paid one
    // page at a time. Measured separately because it is not a fraction of the sequential rate.
    const double refault_mibs = hw.storage.refault_mibs;

    // The rate the expert lane will really see: at the request size this model's slices ARE, at the
    // lane count that reached it. Everything about streaming is priced with this rather than with
    // the surface's peak — pricing the lane at the peak of the surface instead of at the real
    // request size overstated it by 2.2x on one host.
    const uint32_t slice = (uint32_t) model.expert_slice_bytes;
    const uint32_t lanes = slice ? hw.storage.best_lanes(slice) : 0;
    const double stream_mibs = (slice && lanes) ? hw.storage.rate_at(slice, lanes) : 0.0;

    if (host_gibs <= 0.0)
        a.notes.push_back("no compute rate was measured, so residency cannot be priced against a read: every "
                          "group keeps the lane it would have had, and nothing is bought");
    if (refault_mibs <= 0.0)
        a.notes.push_back("the price of a refault was not measured on this storage, so what a mapped group costs "
                          "under pressure is unknown and no residency can be justified by avoiding it");
    if (stream_mibs <= 0.0 && model.is_moe)
        a.notes.push_back("no read rate for this model's expert slice on this storage: the expert lane cannot be "
                          "priced, so no plan that depends on it will be proposed");
    else if (slice && stream_mibs > 0.0) {
        char b[320];
        std::snprintf(b, sizeof(b),
                      "this model's expert slice is %u KiB and at that request size this storage gives %.0f MiB/s "
                      "over %u lanes: the expert lane is priced there, not at the surface's peak",
                      slice >> 10, stream_mibs, lanes);
        a.notes.push_back(b);
        // A rate that is not repeatable makes every number downstream of it unrepeatable too, and a
        // reader comparing two runs deserves to know that before concluding anything from them.
        if (hw.storage.rate_spread > 1.25) {
            std::snprintf(b, sizeof(b),
                          "that rate varied by %.1fx across the probe's own repeats, so every predicted cost below "
                          "carries at least that much uncertainty: treat the ranking as firmer than the totals",
                          hw.storage.rate_spread);
            a.notes.push_back(b);
        }
    }

    // ── 1. price every legal lane ────────────────────────────────────────────────────────────
    //
    // A group left mapped is priced as if pressure WILL drop its pages. That is the pessimistic end
    // of a range this cannot narrow without knowing the final allocation, and the bias it introduces
    // pushes toward residency — which the budget bounds anyway, so the error cannot spend memory
    // that does not exist.
    struct Candidate {
        WeightGroup group;
        Lane lane;
        uint64_t bytes;  // RAM it wants
        double saved;    // seconds/token saved against this group's baseline lane
        double density;  // saved per byte of RAM: the ranking key
        bool fractional; // may take part of `bytes` and save proportionally (the expert cache)
        uint64_t floor;  // fractional only: below this it saves nothing at all
        std::string why;
    };
    std::vector<Candidate> candidates;

    const Lane resident_lane = in.can_pin ? Lane::Pinned : Lane::Resident;

    for (int i = 0; i < (int) WeightGroup::count; ++i) {
        const WeightGroup g = (WeightGroup) i;
        const GroupDemand & d = model.group(g);
        if (d.bytes == 0) continue;
        GroupPlacement & p = a.at(g);

        const double compute = seconds_at_gibs(d.bytes_per_token, host_gibs);
        const double refault = seconds_at_mibs(d.bytes_per_token, refault_mibs);

        // The baseline: mapped, and refaulted. Both terms, because the bytes must be read AND
        // multiplied; a group that is resident still pays the compute.
        p.seconds_worst_case = finite_or_zero(compute) + refault;
        p.seconds_per_token = p.seconds_worst_case;
        p.reason = "left mapped: nothing cheaper fit in the budget";

        if (g == WeightGroup::Experts) {
            if (!model.is_moe || stream_mibs <= 0.0) continue;
            const uint64_t cycle = model.token_cycle_bytes;
            if (cycle == 0) continue;

            // Streaming with an EMPTY cache already beats faulting the whole set through the page
            // cache: a slice is read once, sequentially, at the size the surface was measured at,
            // instead of a page at a time. So this is the group's new baseline, not a candidate.
            p.lane = Lane::ExpertStream;
            p.seconds_per_token = seconds_at_mibs(cycle, stream_mibs) + finite_or_zero(compute);
            p.reason = "streamed, no cache: the routed top-k read as sequential slices";

            // Two floors, different in kind. The model's token cycle is the MECHANICAL one: below
            // it an LRU evicts a slice before the same token needs it again, so the cache costs its
            // memory and returns no hits. The engine additionally refuses any budget under a fixed
            // guard, which is model-independent and predates this; a plan has to clear both or the
            // engine would reject the configuration it just produced.
            const uint64_t floor = std::max(cycle, in.engine_cache_min);
            if (in.engine_cache_min > cycle)
                a.notes.push_back("the expert cache floor here is the engine's fixed guard (" +
                                  mibs(in.engine_cache_min) + "), not this model's token cycle (" + mibs(cycle) +
                                  "): the guard is the binding constraint");

            // A cache holding the whole expert set saves every read. The credited hit rate is
            // linear in its size, so the saving per byte of RAM is constant above the floor and this
            // candidate can be ranked directly against the read-whole groups.
            const double saved_full = seconds_at_mibs(cycle, stream_mibs) * std::min(1.0f, pol.cache_hit_optimism);
            if (saved_full > 0.0)
                candidates.push_back({g, Lane::ExpertStream, d.bytes, saved_full, saved_full / (double) d.bytes, true,
                                      floor, "expert cache"});
            continue;
        }

        if (d.row_gatherable)
            p.reason = "left mapped: the graph gathers rows from it, so a token reads a few KiB of it however "
                       "large the table is, and residency would buy bytes nothing reads";

        if (d.row_gatherable && refault_mibs > 0.0) {
            // A table the graph only gathers rows from is resident for bytes that are never read.
            // Streaming its rows costs one small read per token and returns the rest of its
            // footprint to the groups that ARE read whole. It wants a window, not residency, so it
            // never competes for the budget.
            const double row_cost = seconds_at_mibs(d.bytes_per_token, refault_mibs) + finite_or_zero(compute);
            if (row_cost < p.seconds_per_token) {
                p.lane = Lane::RowStream;
                p.seconds_per_token = row_cost;
                p.reason = "row-gathered: only the rows the graph asks for are read, so residency would buy bytes "
                           "nothing reads";
                continue;
            }
        }

        // Residency removes the refault and leaves the compute. Where either half is unmeasured the
        // saving is not a number, and a candidate without a number does not enter the ranking.
        if (!std::isfinite(p.seconds_worst_case) || !std::isfinite(compute)) continue;
        const double saved = p.seconds_worst_case - compute;
        if (saved <= 0.0) continue;
        candidates.push_back(
            {g, resident_lane, d.bytes, saved, saved / (double) d.bytes, false, 0, "read whole, every token"});
    }

    // ── 2. spend the budget down the ranking ─────────────────────────────────────────────────
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate & x, const Candidate & y) { return x.density > y.density; });

    uint64_t remaining = in.budget_bytes;
    for (const Candidate & c : candidates) {
        GroupPlacement & p = a.at(c.group);
        if (c.fractional) {
            if (remaining < c.floor) {
                a.notes.push_back("expert cache left off: " + mibs(remaining) + " remained but one token's routing " +
                                  "reads " + mibs(c.floor) +
                                  ". A budget below that evicts a slice before the same token needs it again - an "
                                  "eviction per read and no hits, measurably slower than no cache at all. The floor "
                                  "is the model's, not a constant.");
                continue;
            }
            // Rounded down to a whole MiB: the flag this becomes is expressed in MiB, and a report
            // that says 1749 while the command line says 1748 is a report nobody can check against
            // the run it produced.
            const uint64_t take = std::min(remaining, c.bytes) & ~((1ull << 20) - 1);
            if (take < c.floor) continue;
            a.cache_bytes = take;
            remaining -= take;

            // The lane is settled here; the SIZE is not final until the reservation below has had
            // its say, so the rationale is written there. A reason quoting a number the placement
            // does not carry is a plan that contradicts itself, which is worse than a terse one.
            p.lane = Lane::ExpertStream;
            p.resident_bytes = take;
            continue;
        }
        if (c.bytes > remaining) continue; // does not fit; a later, smaller candidate still might
        remaining -= c.bytes;
        p.lane = c.lane;
        p.resident_bytes = c.bytes;
        p.seconds_per_token = p.seconds_worst_case - c.saved;
        p.reason = std::string(c.lane == Lane::Pinned ? "pinned: " : "resident: ") + c.why;
    }

    // The cache, sized against what the loader will actually leave. Two paths, and which one ran is
    // the difference between a number that was chosen and one that was merely available.
    //
    // Where the expert lane could be PRICED, the ranking above already decided what the cache is
    // worth and `a.cache_bytes` holds it; all that is left is to bound it by what the dense policy
    // is about to take, since that mode is one global setting the ranking cannot override.
    //
    // Where it could NOT be priced, sizing the cache is still not a guess to be refused: the regime
    // decided that the experts stream, so the cache must have some size, and the honest answer is
    // what remains. That is the old residual - kept, but now clearly labelled as the unpriced
    // fallback rather than presented as a decision.
    const uint64_t after_reserved = in.budget_bytes > in.reserved_bytes ? in.budget_bytes - in.reserved_bytes : 0;
    if (in.cacheable_bytes > 0 && model.is_moe) {
        const uint64_t priced = a.cache_bytes;
        uint64_t sized = std::min(after_reserved, in.cacheable_bytes) & ~((1ull << 20) - 1);

        // The floor applies to whatever survived the reservation, not only to what the ranking
        // proposed. A cache under one token cycle evicts what the same token still needs, so it
        // costs its memory and returns no hits - and a bound that only guards one of the two paths
        // into this number is not a floor.
        const uint64_t floor = std::max(model.token_cycle_bytes, in.engine_cache_min);
        if (sized > 0 && sized < floor) {
            a.notes.push_back("expert cache left off: " + mibs(sized) + " survived the dense reservation but one " +
                              "token's routing reads " + mibs(floor) +
                              ". Under that floor the cache evicts a slice before the same token needs it again - "
                              "an eviction per read and no hits, measurably slower than no cache at all.");
            sized = 0;
        }

        if (priced > 0) {
            sized = std::min(priced, sized);
        } else if (sized > 0) {
            a.notes.push_back("the expert cache was sized from what remains after the dense policy (" + mibs(sized) +
                              ") rather than from what a hit is worth: this machine's read rate was not measured, "
                              "so there is no price to rank it against. It is the answer that was available, not "
                              "the answer that was chosen.");
        }
        a.cache_bytes = sized;
        GroupPlacement & p = a.at(WeightGroup::Experts);
        if (sized > 0) {
            p.lane = Lane::ExpertStream;
            p.resident_bytes = sized;

            // The cost and the rationale, both from the size that was actually chosen.
            const double held = (double) sized / (double) in.cacheable_bytes;
            const double hit = std::min(1.0, (double) pol.cache_hit_optimism * held);
            p.seconds_per_token =
                seconds_at_mibs((uint64_t) ((1.0 - hit) * (double) model.token_cycle_bytes), stream_mibs) +
                finite_or_zero(seconds_at_gibs(model.group(WeightGroup::Experts).bytes_per_token, host_gibs));
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "streamed with a %s cache, %.0f%% of the expert set; credited hit rate %.0f%%, which is "
                          "conservative on purpose and is the coefficient a run's own counters correct",
                          mibs(sized).c_str(), 100.0 * held, 100.0 * hit);
            p.reason = buf;
        }
    }

    // ── 3. offer each resident group to a device ─────────────────────────────────────────────
    //
    // Only RESIDENT groups are candidates. On a streamed group the read dominates the compute by
    // more than an order of magnitude, so a faster device would hide behind the same I/O and buy
    // nothing — and the memory it consumed would come out of the residency that does pay.
    //
    // Two gates before speed is even looked at. A device that does not reproduce the CPU's answer
    // is excluded on CORRECTNESS. And the crossings the move creates must be PRICED: dense and
    // expert halves alternate per layer, so a split placement crosses the boundary twice per layer,
    // and a bandwidth ratio cannot see any of it. Where that price is unmeasured this declines,
    // which is the whole difference between a planner and an oracle.
    const bool split_priced = pol.device_split_seconds > 0.0;
    bool any_device = false;
    bool wanted_device = false;

    for (int i = 0; i < (int) WeightGroup::count; ++i) {
        GroupPlacement & p = a.groups[i];
        const GroupDemand & d = model.group((WeightGroup) i);
        if (d.bytes == 0 || host_gibs <= 0.0) continue;
        const bool resident = p.lane == Lane::Resident || p.lane == Lane::Pinned || p.lane == Lane::MmapWarm;
        if (!resident) continue;

        const double t_host = seconds_at_gibs(d.bytes_per_token, host_gibs);
        const ComputeDevice * best = nullptr;
        double best_t = t_host;

        for (size_t di = 0; di < hw.devices.size(); ++di) {
            const ComputeDevice & dev = hw.devices[di];
            if (dev.is_cpu || dev.memory_bandwidth_gibs <= 0.0) continue;
            if (dev.identity_ok != Tri::Yes) continue; // correctness first, and it must be a fact
            if (dev.needs_repack == Tri::Yes) continue;
            // A device that cannot hold the group is not an option; and on a device whose memory IS
            // the host's the group's bytes are the SAME bytes, so counting them twice is how a
            // planner allocates memory that does not exist.
            if (!dev.host_memory && dev.memory_free && dev.memory_free < d.bytes) continue;

            const double t = seconds_at_gibs(d.bytes_per_token, dev.memory_bandwidth_gibs);
            if (t < best_t * (1.0 - pol.min_backend_win)) {
                best = &dev;
                best_t = t;
            }
        }
        if (!best) continue;
        wanted_device = true;

        // The crossings this creates: the boundary is entered and left once per layer that has a
        // group on each side, and the experts are always on the host today.
        const uint32_t crossings = 2 * std::max(1u, model.n_moe_layer);
        if (!split_priced) {
            a.notes.push_back(
                std::string("a device would consume this model's ") + group_name((WeightGroup) i) + " faster (" +
                std::to_string((int) std::lround(best->memory_bandwidth_gibs)) + " against " +
                std::to_string((int) std::lround(hw.host_bandwidth_gibs)) +
                " GiB/s), and it is NOT placed there: the move puts the expert half on the host and this half on the "
                "device, which crosses the boundary twice per layer (" +
                std::to_string(crossings) +
                " times a token), and what a crossing costs is unmeasured on this machine. A bandwidth ratio cannot "
                "see that cost: on a phone measured at 20 against 12 GiB/s the same move ran at 0.53x, and a "
                "contiguous-block placement - the shape that should have put the boundary in one place - went 2.398 "
                "to 0.587 tok/s. An unmeasured number may not justify a move.");
            continue;
        }
        const double split_cost = pol.device_split_seconds * crossings;
        if (t_host - best_t <= split_cost) {
            a.notes.push_back(std::string("a device consumes this model's ") + group_name((WeightGroup) i) +
                              " faster, and the graph crossings the move creates cost more than it saves");
            continue;
        }
        for (size_t di = 0; di < hw.devices.size(); ++di)
            if (&hw.devices[di] == best) p.device = (int) di;
        p.seconds_per_token -= (t_host - best_t) - split_cost;
        p.reason += "; computed on " + best->name;
        any_device = true;
    }

    // Every non-host choice above is a recommendation until the engine can bind a tensor to a
    // device buffer for a group the streamer serves. Saying so is more useful than dropping it.
    a.device_applicable = !any_device;
    if (any_device)
        a.notes.push_back("a device won at least one group, but this build computes on the host: the placement is "
                          "reported, not applied");
    if (!wanted_device && !hw.devices.empty() && hw.device_local_memory() == 0)
        a.notes.push_back("every device here reads the host's own memory, so moving a weight onto one frees "
                          "nothing: the capacity tier is inert and only bandwidth was left to compare");

    for (const ComputeDevice & dev : hw.devices) {
        if (dev.is_cpu) continue;
        if (dev.identity_ok == Tri::No)
            a.notes.push_back(dev.name + " did not reproduce the CPU's result on the probe graph, and was excluded "
                                         "on correctness rather than on speed");
        if (is_yes(dev.host_ptr_buffers) && dev.host_ptr_verified == Tri::No)
            a.notes.push_back(dev.name + " advertises host pointers and did not honour them when handed one: no "
                                         "streamed group could ever be rebound there, whatever its speed");
    }

    // ── totals ───────────────────────────────────────────────────────────────────────────────
    for (int i = 0; i < (int) WeightGroup::count; ++i) {
        if (model.groups[i].bytes == 0) continue;
        a.seconds_per_token += finite_or_zero(a.groups[i].seconds_per_token);
        a.seconds_worst_case += finite_or_zero(a.groups[i].seconds_worst_case);
        a.resident_bytes += a.groups[i].resident_bytes;
    }
    return a;
}

} // namespace bmoe
