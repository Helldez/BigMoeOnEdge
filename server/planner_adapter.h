// The hardware planner, as the server offers it.
//
// The planner is a library the engine may or may not carry (see server/CMakeLists.txt). With it,
// this makes the plan with the same call as `bmoe-cli --auto` (bmoe/auto_plan.h) and turns the Plan
// into what the API returns: its decisions, and the parameter values it would set, expressed through the
// parameter table so they land in the same settings layers as everything else. Without it, every
// call says so.
#pragma once

#include "json_util.h"

#include "bmoe/auto_plan.h"
#include "bmoe/calibrate.h"
#include "bmoe/config.h"
#include "bmoe/measured_knobs.h"
#include "bmoe/routing.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bmoe::server {

// What a plan was made from, for the rules to run again on (bmoe/auto_plan.h, replan): the probed
// facts, the config the plan was asked for and the keys the operator pinned. A load-time calibration
// plans on it after each candidate, with the model unloaded, without probing the machine again.
struct PlanBasis {
    AutoPlan facts;
    RunConfig cfg;
    PlanRequest req;
};

struct PlanOutcome {
    bool ok = false;
    std::string error;                         // why there is no plan (no planner, no model)
    json body;                                 // the /api/plan object
    std::map<std::string, std::string> values; // parameters the plan changes, by key
    json decisions = json::array();
    // What the plan's memory ledger charges the run with (Ledger::held): the session's own account
    // of the memory it holds once loaded, which a question about loading another model gives back.
    uint64_t held_bytes = 0;
    // The machine the plan measured (hardware_fingerprint): what a knob verdict measured under this
    // plan is kept against.
    std::string hardware;
    // Its place in the order this server measured plans in (0: no plan): the load's plan is the one
    // the model runs with only while no measurement followed it (plan_view.h, held_plan).
    uint64_t seq = 0;
    // What it was made from; null without a planner.
    std::shared_ptr<const PlanBasis> basis;

    // Whether it leaves a decision across loads to time (bmoe/calibrate.h, side_pending).
    bool side_pending() const;
    // Whether a calibration has anything to do: a pending side, or a side that failed to open before
    // and is due to be tried again (bmoe/calibrate.h, calibration_due).
    bool calibration_due() const;
    // Whether the side opposite the pending one has a rate in the record: it opened and decoded here.
    bool other_side_measured() const;
};

// `plan` with its pending side counted as one that failed to open: the plan on the other side,
// decided. What a served load opens instead of a side not proven to open, and what it falls back to
// when the pending side's open fails. Not ok when nothing is pending, or when the other decision
// then has a side pending too (there is no side known to open).
PlanOutcome plan_other_side(const PlanOutcome & plan);

bool planner_available();

// Plan `cfg` (its model must be set) on this machine. `pinned` are the operator's keys, which the
// planner leaves alone. Probes the machine: a second or two of storage reads and a bandwidth run.
// With `isolated_probes`, each compute device beyond the host is first asked in a process of its
// own (this executable again, with --probe-device), so a driver that ends the process it is used
// from marks the device unusable instead of ending the server; where a second process cannot be
// started the probes run here. `on_step` (nullable) hears each stage of the measurement as it
// starts: its key (bmoe/auto_plan.h, k_plan_steps), its words for a person, its position and how
// many there are.
using PlanStepFn = std::function<void(const std::string & step, const std::string & label, int index, int count)>;
// `routing` (nullable) is the model's own recorded routing from an earlier load (routing_store.h):
// with it the cache is priced on the model's hit curve. `knobs` (nullable) are the knob verdicts kept
// for the model; with `measure_knobs` the plan arms the first answer to measure what is not measured
// yet, for the caller to keep.
PlanOutcome make_plan(const RunConfig & cfg,
                      const std::vector<std::string> & pinned,
                      bool isolated_probes = true,
                      const PlanStepFn & on_step = nullptr,
                      const RoutingTrace * routing = nullptr,
                      const std::vector<MeasuredKnobs> * knobs = nullptr,
                      bool measure_knobs = false);

// A load-time calibration of `plan` (bmoe/calibrate.h), on the facts it was made from: `start` is the
// record of measurements to start from (what this machine kept for the model, which a served answer
// may have grown since the plan), and `run` opens each candidate. `on_step` hears each candidate as it
// starts, as make_plan's does. With the model unloaded: the candidates need the memory it holds.
struct CalibrationOutcome {
    bool ran = false;      // a calibration ran (the plan had a side pending, and there is a planner)
    bool aborted = false;  // a candidate was stopped before its verdict
    bool complete = false; // no side is left pending
    PlanOutcome plan;      // the plan on the final record, to load
    MeasuredKnobs knobs;   // that record, for the caller to keep
};
CalibrationOutcome calibrate_plan_outcome(const PlanOutcome & plan,
                                          const MeasuredKnobs & start,
                                          const RunCandidate & run,
                                          const PlanStepFn & on_step = nullptr);

// The child's side of an isolated probe: probe `device` for `model_path`, print its facts after
// the marker, and return the process's exit code. 3 when there is no such device or no planner.
int run_device_probe_child(const std::string & model_path, const std::string & device);

} // namespace bmoe::server
