// Load-time calibration: the decisions across loads, timed before the plan is served.
//
// Two plan decisions are fixed when the model or its context is created, so one load cannot try both
// sides: whether a device with memory of its own holds a cache of experts or the capacity fitter's
// whole layers (MeasuredKnobs::device_cache_tps), and how wide the graph is where even a graph held
// to the device bound (PlannerPolicy::ubatch_max_device) takes more of the card's memory than its share
// (MeasuredKnobs::width_narrow). Timed across loads, each side costs a whole session of a person's use
// at whatever rate it decodes: measured on rented cards, a model's second load decoded at 37 tok/s
// instead of 68 on a 12 GB card and at 35 instead of 84 on a 32 GB one. So a load that has such a decision pending
// opens each side as a short candidate load of its own first, times its decode, and serves the winner.
//
// The planner stays the oracle. A side is pending exactly when the plan sets one
// (KnobTrialConfig::device_tier or width_side, side_pending()); calibrate() runs that candidate,
// puts its rate in the record with the mapping a session's own timing uses (record_side_rate), and
// asks the planner again on the grown record, until no side is pending. Every rule the across-loads
// timing has holds as it is: a fitter's placement whose decode ceiling cannot win is never timed, a
// win needs the margin. A candidate that cannot be opened is recorded as not viable, and the plan then
// treats that side as the loser.
//
// Pure policy: no llama.cpp, no clock. Planning and running a candidate are ports (Replan,
// RunCandidate); the front-end composes them (bmoe/auto_plan.h, calibrate_plan), and the engine's
// candidate is run_calibration_candidate (bmoe/session.h). See docs/measured-planning.md.
#pragma once

#include "bmoe/config.h"
#include "bmoe/measured_knobs.h"
#include "bmoe/plan.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bmoe {

// The decisions a calibration times, each with two sides: the device tier, then the graph width.
inline constexpr int k_calibration_decisions = 3;
inline constexpr int k_calibration_sides = 2;

// What one candidate run is asked to do.
struct CalibrationRun {
    int index = 0;    // its place among the runs of this calibration, 0-based
    int runs_max = 0; // the most runs this calibration makes
    // The token ids the first run decoded, which this one decodes again teacher-forced so both sides
    // route alike; empty for the first run, which samples them (and returns them).
    std::vector<int32_t> replay;
    // The other side of the same decision, timed before: its fastest and slowest timed step, in
    // seconds per token (RateProbe::set_reference). 0 when that side has not run: this one is timed
    // to its end.
    double reference_lo = 0.0, reference_hi = 0.0;
};

// What a candidate run measured.
struct CandidateTiming {
    bool created = false; // the candidate loaded; false: not viable here, and `error` says why
    std::string error;
    bool aborted = false;                // stopped from outside before a verdict: nothing is recorded
    double tokens_per_second = 0.0;      // its decode rate, 0 when it decoded too little to tell
    double step_lo = 0.0, step_hi = 0.0; // its fastest and slowest timed step, seconds per token
    int windows = 0;                     // timed windows behind the rate
    bool cut = false;                    // stopped early, past the reference's range by the margin
    std::vector<int32_t> tokens;         // the ids it decoded, sampled or replayed
};

// Plan again on the same facts with this record of measurements.
using Replan = std::function<Plan(const MeasuredKnobs &)>;
// Load `config` as a candidate, time a short decode, and close it.
using RunCandidate = std::function<CandidateTiming(const RunConfig &, const CalibrationRun &)>;

// How many failed opens of a side a calibration still tries again (MeasuredKnobs::*_unviable). A
// side that failed once is tried at the next calibration: the failure may have been the machine's
// state at that moment, another program holding the card's memory or a driver's hiccup. Failing on a
// second calibration as well is taken as the side not fitting this machine with this engine, and it
// is not tried again, since each try costs a load that fails, sometimes after minutes. A new engine
// version keeps a record of its own, and tries again.
inline constexpr int k_unviable_retries = 2;

struct CalibrationPolicy {
    // The most candidates opened: every side of every decision once. A plan that keeps asking past
    // this is left to the across-loads timing.
    int runs_max = k_calibration_decisions * k_calibration_sides;
    // A side is offered again while it has failed fewer times than this.
    int unviable_retries = k_unviable_retries;
};

// Whether a calibration of a plan made on `knobs` would open anything: a side the plan leaves pending,
// or one that failed to open before, is under the retry budget, and that the plan asks for again once
// its count is set aside (`replan` decides it, as calibrate() would).
bool calibration_due(const Plan & p,
                     const MeasuredKnobs & knobs,
                     const Replan & replan,
                     const CalibrationPolicy & pol = {});

struct CalibrationResult {
    Plan plan;             // the plan on the final record: the one to serve
    MeasuredKnobs knobs;   // the record it started from with every rate measured here put in
    int runs = 0;          // candidates opened (a configuration already timed is not opened again)
    bool complete = false; // no side is pending: the served plan is the decided one
    bool aborted = false;  // a run was stopped from outside
};

// Whether `p` has a side of a decision across loads still to time.
bool side_pending(const Plan & p);

// Whether a served load may open `p` as it is: nothing pending, or only a side that every other load
// of the plan already opened with: the graph at the plan's own width, or the device cache with every
// miss uploaded (the narrow graph and a CPU share are configurations no load has run yet). What a
// server serves instead of an unproven side once the other side has been measured: on a 32 GB card the plan on the
// measured device cache named the width's wide side, which counted as a side still to prove, so the load served the
// unproven placement and decoded at 42 tok/s against the cache's 103.
bool servable(const Plan & p);

// The candidate a pending plan runs: its config with the knob trial's threads and overlap cleared
// (they switch within a load and are measured on the served one) and no side named, since the
// candidate times itself.
RunConfig calibration_candidate(const RunConfig & planned);

// Time every side `first` leaves pending, one candidate at a time, replanning on the record after
// each, until none is pending, a run is aborted, or the plan stops gaining from the runs (it asks for
// a measurement a short decode cannot make, such as the prefill of long prompts, which the served
// load then times as before). `start` is the record the plan was made on.
CalibrationResult calibrate(const Plan & first,
                            const MeasuredKnobs & start,
                            const Replan & replan,
                            const RunCandidate & run,
                            const CalibrationPolicy & pol = {});

} // namespace bmoe
