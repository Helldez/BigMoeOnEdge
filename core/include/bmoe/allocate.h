// The cost model and the allocation: which memory each group of weights gets, and who computes on
// it, chosen by what it is worth rather than by what fits.
//
// This is the part of planning that is a TRADE rather than a reading. Most knobs in a plan are
// decided by one fact — the lane count by the storage curve, o-direct by whether uncached reads
// return correct bytes, the thread count by a sweep — and for those a rule that reads the fact is
// the right shape. The four decisions here are not like that: RAM given to the dense set is RAM
// taken from the expert cache, a group moved to a device frees host memory and adds graph
// crossings, and a narrower prefill hands its reservation to the weights. They compete, so they
// have to be priced against each other in ONE unit.
//
// That unit is SECONDS PER BYTE OF WEIGHTS. It is available because batch-1 decode is
// bandwidth-bound: every weight is read once and multiplied once, so a GEMV's cost tracks the bytes
// it sweeps exactly as a read's does, and flash, RAM and device memory become directly comparable.
// It is what lets the flash tier enter the objective function at all — which is the one thing no
// other placement solver does.
//
// Three rules keep this from becoming an oracle, and they are load-bearing rather than decorative:
//
//   1. An unmeasured number may never justify a move. A missing fact makes a candidate ineligible,
//      never cheap. Every decline says which measurement it was owed.
//   2. A non-CPU backend must win by a MARGIN, not merely win. Half the plausible levers this
//      engine has tried lost on measurement; a planner built to believe its own model would
//      rediscover every one of them with confidence.
//   3. The allocation that streams nothing must be reachable, because residency beats streaming
//      whenever residency is available.
//
// Pure policy: no llama.cpp, no I/O, no clock. Driven against synthetic machines in the tests.
#pragma once

#include "bmoe/hardware_profile.h"
#include "bmoe/model_profile.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bmoe {

// How a group's bytes reach the compute. Not every lane can serve every group: GroupDemand's
// `streamable` and `row_gatherable` say which are legal, and the allocator never proposes a lane
// the engine cannot execute.
enum class Lane {
    Mmap,         // left mapped; the kernel serves it and may drop it, refaulting a page at a time
    MmapWarm,     // left mapped, but paged in once at load so decode never faults it
    Resident,     // copied into our own anonymous buffers; a reclaim costs whatever a reclaim costs here
    Pinned,       // as Resident, in a store the kernel may not take back (platform-gated)
    RowStream,    // address space only; the rows the graph gathers are pulled from flash
    ExpertStream, // the routed top-k slices are read from flash into a bounded cache
    count,
};

const char * lane_name(Lane l);

// One group's decision, with the arithmetic that produced it kept attached. `reason` is not a log
// line: it is the part a person reads to decide whether they believe the plan.
struct GroupPlacement {
    WeightGroup group = WeightGroup::Other;
    Lane lane = Lane::Mmap;

    // Index into HardwareProfile::devices, or -1 when the group's compute stays on the host. A
    // non-host index is a RECOMMENDATION until the engine can express it; see `device_applicable`.
    int device = -1;

    uint64_t resident_bytes = 0;   // RAM this placement holds for the whole run
    double seconds_per_token = 0;  // predicted contribution to one decoded token
    double seconds_worst_case = 0; // the same group with nothing resident and every byte refaulted
    std::string reason;
};

// What the allocator is allowed to spend and what it must leave alone.
struct AllocationInputs {
    uint64_t budget_bytes = 0;     // RAM this plan may commit, after margins and reservations
    uint64_t engine_cache_min = 0; // the engine's own fixed cache guard, which a plan must also clear
    bool can_pin = false;          // a reclaim-exempt store exists and is large enough to matter

    // Bytes the dense policy will hold whatever this ranking concludes. The engine's dense mode is
    // one setting for every non-expert tensor, so per-group dense residency is not something it can
    // be told; the mechanism is chosen upstream from what a reclaim COSTS here, and this is the
    // consequence. The ranking below still prices each group — that analysis is what the plan
    // reports and what a future per-group lane would act on — but the cache is sized against what
    // is actually left, or the plan would promise memory the loader is about to take.
    uint64_t reserved_bytes = 0;

    // The expert bytes the streamer will actually serve: the cache is never worth more than the set
    // it caches, and on a machine where the first stage placed some layers on a device that set is
    // smaller than the model's.
    uint64_t cacheable_bytes = 0;
};

struct Allocation {
    GroupPlacement groups[(int) WeightGroup::count];

    uint64_t cache_bytes = 0;    // expert cache budget; 0 means the expert lane is not used
    uint64_t resident_bytes = 0; // what the whole allocation holds
    uint64_t budget_bytes = 0;

    double seconds_per_token = 0; // what the allocation predicts

    // What the same token would cost with nothing resident and every byte refaulted a page at a
    // time. An UPPER BOUND on what the allocation avoids, not a prediction of what mmap would
    // really do — the page cache retains some of the model and readahead amortises some of the rest,
    // both by an amount this cannot know. Reported as a bound, never divided into the prediction to
    // manufacture a speedup figure.
    double seconds_worst_case = 0;

    // True when every non-host device choice above can actually be executed by this build. False
    // means the allocation REPORTS a placement it cannot apply, which is said out loud rather than
    // silently dropped: a recommendation the engine cannot honour is still the most useful thing
    // the probe found.
    bool device_applicable = true;

    std::vector<std::string> notes;

    const GroupPlacement & at(WeightGroup g) const { return groups[(int) g]; }
    GroupPlacement & at(WeightGroup g) { return groups[(int) g]; }
};

struct PlannerPolicy; // bmoe/planner.h

// Price every legal lane for every group, rank the candidates by seconds saved per byte of RAM, and
// spend the budget down that list. Never fails: a machine with no measurements yields an allocation
// that leaves everything mapped and says, per group, which measurement it was owed.
Allocation allocate(const HardwareProfile & hw,
                    const ModelProfile & model,
                    const AllocationInputs & in,
                    const PlannerPolicy & pol);

} // namespace bmoe
