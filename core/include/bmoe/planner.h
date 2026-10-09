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

    // Fraction of the LOCKABLE total left unasked-for, and it is not there to absorb a misreading:
    // a refusal is survivable, the budget steps down to what is granted. It is there because the
    // platform's limit is not a place to run. What is locked is taken from everything that is
    // not - this process's own context and compute buffers included - and a run that sits on the
    // limit leaves those to be reclaimed. Measured on one 16 GB machine, same model and cache hit
    // rate: 23.1 tok/s asking for 95% of the total, 7.4 and 14.6 asking for 98%.
    float lock_margin = 0.05f;

    // How much room a model must leave BEYOND itself before residency is called safe, as a multiple
    // of what it would hold. Fitting and being left alone are different claims: on a machine whose
    // reclaim can take pages back, a model that fits just barely is reclaimed from underneath and
    // refaults its weights one page at a time, which is measured at 0.1 tok/s against 5.0 for the
    // same model streamed. "Leave as much as you take" is the cheapest test that separates the two
    // without a measurement; the headroom probe replaces it with one. It does not apply where
    // nothing can take the memory back, nor where the limit is a hard cap the margin already sizes.
    float fits_air_ratio = 1.0f;

    // Whether THIS ENGINE can hand a device the memory a streamed expert is made of. It is a fact
    // about our own streamer rather than about any machine, which is why it lives here and not in
    // the hardware profile - and it is false, because the streamer reserves address space and
    // commits pages itself, so the bytes a rebound tensor points at were never allocated by, or
    // registered with, any device.
    //
    // The rule that would route expert compute to a device reads this first, and that is deliberate
    // rather than defensive: without it the plan would name a device on a machine where the device
    // would then read memory it cannot see. A knob that is right in principle and wrong in practice
    // is worse than one that is off, so the precondition is written down and the day the streamer
    // allocates through `ggml_backend_dev_buffer_from_host_ptr` this becomes true and one condition
    // flips. The tests exercise both settings.
    bool streamer_serves_device_memory = false;

    // ── the cost model's coefficients ───────────────────────────────────────────────
    // How many of a token's expert reads a cache serves, credited as this multiple of the FRACTION
    // OF THE EXPERT SET IT HOLDS. 1.0 is the neutral reading: hold a tenth, serve a tenth.
    //
    // The relationship is really super-linear, because routing has strong temporal locality — the
    // one measurement on record is a cache holding 13% of the set serving 61.9% of the reads — so
    // this default is conservative by roughly 5x and deliberately so: over-crediting the cache
    // spends memory on a saving that cannot be collected, and the error is asymmetric. Fitting a
    // curve to a single point would be inventing the shape, so the shape stays linear and the
    // coefficient is what a closed loop corrects from a run's own counters. An earlier 0.22 here
    // was 20x low against that same measurement and made the cache look nearly worthless, which on
    // a machine where cache and residency compete evenly is a mis-allocation rather than caution.
    float cache_hit_optimism = 1.0f;

    // How much faster than the host a device must be before its win is believed, as a fraction. A
    // tenth, not a hair: the two figures come from the same probe on the same machine, and claiming
    // "more" for a difference inside its own repeatability is how a rationale starts being read as
    // noise. Measured 19 against 19 on a phone, which is not a finding.
    float min_backend_win = 0.10f;

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
