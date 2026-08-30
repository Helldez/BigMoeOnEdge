// The planner: (machine, model, request) -> a resolved run, with its reasons.
//
// A pure function. No I/O, no clock, no llama.cpp, no environment — so it can be driven against a
// synthetic machine in a unit test, which is the only way a claim like "this holds on hardware we
// do not own" can be checked at all. The adapters that measure a HardwareProfile and read a
// ModelProfile are separate on purpose; this file must stay testable without either.
//
// The two invariants the rules are written to keep, and which the tests check directly:
//
//   1. Decline instead of guessing. Every knob whose deciding fact is Unknown keeps its default
//      and says so (Source::Unprobed). No rule invents a number from a platform name — there are
//      no platform names in its inputs to invent one from.
//
//   2. Nothing lossy arms itself. Expert dropping, substitution and route-ahead change the output
//      and make a run irreproducible; they are reachable only through PlanRequest::quality_budget,
//      which the caller sets and the planner never raises.
//
// What this deliberately does NOT do: place tensors across compute devices. That is a capacity
// problem, it is exactly solvable from tensor sizes, and llama.cpp already solves it. This planner
// owns the tier below — what to do when the bytes fit nowhere and have to come off flash.
#pragma once

#include "bmoe/hardware_profile.h"
#include "bmoe/model_profile.h"
#include "bmoe/placement.h"
#include "bmoe/plan.h"

namespace bmoe {

// Resolve a run. `base` supplies the caller's starting point (model path, prompt, and any knob it
// set); `request.pinned` names the knobs the planner may not touch. Never fails: an unreadable
// model or an unprofiled machine yields a plan that declines streaming and explains why.
Plan plan_run(const RunConfig & base,
              const HardwareProfile & hw,
              const ModelProfile & model,
              const PlanRequest & request);

// As above, composed over the first stage: `placement` is what llama.cpp's capacity fitter decided
// (which layers keep experts on the host, how much host memory the placed model takes). The
// second stage sizes itself on what is left. A placement with fitted=false is the four-argument
// form: everything on the host, nothing placed.
Plan plan_run(const RunConfig & base,
              const HardwareProfile & hw,
              const ModelProfile & model,
              const Placement & placement,
              const PlanRequest & request);

// Margins the planner applies as POLICY rather than measurement — how much of the machine to leave
// for everything that is not us. They are exposed here so they are visible and overridable instead
// of buried, and so a test can state what it expects. They are not measured constants and must not
// be presented as any: what IS measured is that the cost of getting this wrong is asymmetric, since
// overcommitting a device costs far more than a slightly smaller cache.
struct PlannerPolicy {
    // Fraction of the residency budget kept free, by what losing memory costs on this machine.
    float margin_when_compressed = 0.10f; // a reclaim is cheap: decompression, no I/O
    float margin_when_swapped = 0.04f;    // a reclaim writes to disk, but the process survives
    float margin_when_killed = 0.20f;     // exceeding the share is fatal: stay well inside it
    float margin_when_unknown = 0.15f;    // no fact: the conservative of the three

    // Never leave less than this free, whatever the fractions say on a small machine.
    uint64_t margin_min_bytes = 256ull << 20;

    // How much room a model must leave BEYOND itself before residency is called safe, as a multiple
    // of what it would hold. Fitting and being left alone are different claims: on a machine whose
    // reclaim can take pages back, a model that fits just barely is reclaimed from underneath and
    // refaults its weights one page at a time, which is measured at 0.1 tok/s against 5.0 for the
    // same model streamed. "Leave as much as you take" is the cheapest test that separates the two
    // without a measurement; the headroom probe replaces it with one. It does not apply where
    // nothing can take the memory back, nor where the limit is a hard cap the margin already sizes.
    float fits_air_ratio = 1.0f;

    static PlannerPolicy defaults() { return PlannerPolicy(); }
};

// As above, with the margins overridden. The four-argument form calls this with the defaults.
Plan plan_run(const RunConfig & base,
              const HardwareProfile & hw,
              const ModelProfile & model,
              const Placement & placement,
              const PlanRequest & request,
              const PlannerPolicy & policy);

} // namespace bmoe
