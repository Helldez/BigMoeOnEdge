// The whole `--auto` sequence as one call: measure this machine and this model, ask llama.cpp's
// capacity fitter for the first stage, and plan the second.
//
// Every front-end that plans runs exactly this, and it used to be written out in each of them - the
// CLI's --auto and the server's /api/plan carried the same twelve calls in the same order, the same
// cache-floor arithmetic and the same probe timeout, kept in step by hand. The ORDER is a
// measurement rather than a preference (see probe_and_plan below), so two copies of it are two
// chances for one to measure the other's recovery. What a front-end decides is in AutoPlanOptions:
// how intrusive the probes may be, and how this executable is started again to probe a device in a
// process of its own.
//
// This is a composition of the adapters in bmoe/probe.h with the rules in bmoe/planner.h, and it
// lives with the adapters: it starts llama.cpp's backends and touches devices, storage and memory.
// The rules stay pure; nothing here decides a knob.
#pragma once

#include "bmoe/calibrate.h"
#include "bmoe/config.h"
#include "bmoe/hardware_profile.h"
#include "bmoe/model_profile.h"
#include "bmoe/plan.h"
#include "bmoe/routing.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace bmoe {

// How long a device gets to answer its probe in a process of its own. Generous on purpose: the
// child also measures the host as its reference, and a phone that is throttled takes its time. What
// it bounds is the case with no other end - a device that never answers.
inline constexpr double k_isolated_probe_seconds = 120.0;

struct AutoPlanOptions {
    // Measure the storage with the reads this engine issues (probe_storage): a few tens of MiB, well
    // under a second. Off, the plan is made from free facts only and touches no drive, and every
    // knob the storage decides keeps its default as unprobed.
    bool probe_io = true;

    // Measure what this machine lets a process KEEP by holding memory until the kernel takes some
    // back (probe_headroom's active mode), instead of estimating it. Intrusive by nature - seconds
    // of real pressure on the machine - so it is never on unless a person asked for it.
    bool probe_mem = false;

    // How this executable is started again as the child that probes one device: its arguments for
    // `device`, in the front-end's own spelling, the child then calling probe_device_child(). Every
    // device beyond the host is probed that way, so a driver that ends the process it is used from
    // ends the child and marks the device unusable instead of ending the plan. Empty: the devices are
    // probed in this process, as they also are where this platform cannot start a second one.
    std::function<std::vector<std::string>(const std::string & device)> isolated_probe_args;
    double isolated_probe_seconds = k_isolated_probe_seconds;

    // Called as each stage of the sequence starts, with its key (k_plan_steps), its position and
    // the number of stages, so a front-end can say which probe is running instead of a blind wait.
    // Called on the planning thread; empty, nothing is reported.
    std::function<void(const char * step, int index, int count)> on_step;

    // The routing this model recorded on an earlier run (Session::routing_record), kept by the
    // front-end against the model file's fingerprint (model_fingerprint in bmoe/probe.h). With it the
    // cache is priced on the model's own hit curve (bmoe/routing.h); without it, or with one that is
    // not this model's, the plan is the one a first load gets. Not owned; read during the call only.
    const RoutingTrace * routing = nullptr;

    // What earlier loads of this model measured about its lossless knobs (Session::measured_knobs),
    // every record the front-end kept for the file, one per machine and engine. The one whose
    // hardware_fingerprint and engine version are this run's becomes ModelProfile::knobs; the rest
    // are another machine's or another engine's and are not read. Not owned; read during the call.
    const std::vector<MeasuredKnobs> * knobs = nullptr;

    // Calibrate at load (bmoe/calibrate.h): where the plan leaves a decision across loads pending, time
    // each side with this candidate runner before returning, and return the decided plan with what it
    // measured (AutoPlan::calibration). Its stages are reported through on_step as k_calibrate_step.
    // Empty: the plan is returned as it is, and the load it is served on times its side as before.
    RunCandidate calibrate;
};

// The stages probe_and_plan() reports through AutoPlanOptions::on_step, in the order they run. The
// keys are a protocol for front-ends (the server's `plan_progress` event), not words to display.
inline constexpr const char * k_plan_steps[] = {"devices", "model",     "compute", "storage",
                                                "memory",  "placement", "rules"};
inline constexpr int k_plan_step_count = (int) (sizeof(k_plan_steps) / sizeof(k_plan_steps[0]));
// The stage a load-time calibration reports, once per candidate it opens: its index among the runs
// and the most it makes (CalibrationPolicy::runs_max), apart from the stages above since most plans
// have nothing to calibrate.
inline constexpr const char * k_calibrate_step = "calibrate";

// The plan and the two profiles it was made from: a front-end that explains a plan quotes the
// facts (every device's figures, the model's weight groups) beside the decisions. The first
// stage's placement is the plan's own (Plan::placement).
struct AutoPlan {
    HardwareProfile hw;
    ModelProfile model;
    Plan plan;

    // The rest of what the rules ran on: the first stage's placement at the graph width the caller
    // asked for, and the devices the backends were narrowed to. With the profiles, what replan() runs
    // the rules on again.
    Placement placement;
    std::vector<std::string> backends;

    // The capacity fitter's placements at a narrower graph than `placement`'s, by width: the re-fits
    // the rules ask for (replan), each asked of the fitter once and then read from here. A re-fit
    // answers from the same file, devices, context and cache types whatever the record of
    // measurements, so asking again only costs time: on a 24 GB card with GLM-4.7 every re-fit loaded
    // the model's metadata for a dozen dry runs, between every two candidates of a calibration. It is
    // also the right answer: the fitter reads the devices' free memory, which is the machine's as it
    // was planned on only before a candidate or the served model holds any of it. Kept with the facts,
    // so a copy of them (a server's plan basis) carries what was fitted.
    mutable std::map<uint32_t, Placement> placement_at_width;

    // Whether a load-time calibration ran (AutoPlanOptions::calibrate, calibrate_plan), and what it
    // gave: `plan` is then its final plan, and `calibration.knobs` the record for the front-end to keep.
    bool calibrated = false;
    CalibrationResult calibration;
};

// Probe this machine and `cfg.model_path`, then plan `cfg` on them. `req` carries what the caller
// pinned. Never fails: a model that cannot be read or a machine that reports nothing yields a plan
// that declines and says why, exactly as plan_run() does.
//
// QUIET FIRST, DIRTY LAST: backends are registered before anything looks at devices; the devices
// are asked (in their own processes, where the options say how) before this process uses any; the
// bandwidth and device-cost probes run before the storage probe reads gigabytes; the headroom probe
// runs last of all, because in its active mode it leaves the kernel busy and anything measured
// after it would measure the recovery.
AutoPlan probe_and_plan(const RunConfig & cfg, const PlanRequest & req, const AutoPlanOptions & opt = {});

// The rules once more on the facts `facts` was planned on, with `knobs` as the record of measurements:
// the planning half of probe_and_plan, the capacity fitter's re-fits at a narrower width included, and
// no probe. A re-fit at a width the facts already hold is read from them (AutoPlan::placement_at_width),
// so only a width not fitted before asks the fitter. What a calibration plans with after each candidate
// (bmoe/calibrate.h, Replan).
Plan replan(const AutoPlan & facts, const RunConfig & cfg, const PlanRequest & req, const MeasuredKnobs & knobs);

// The rules alone on those facts, without the capacity fitter's re-fits at a narrower width: no
// probe, nothing that reads the machine as it is now, so it may run with a model loaded. Which side
// of a decision across loads is pending is the rules' (the re-fits keep the side the first plan named),
// so this answers whether a calibration would open anything (calibration_due) at the cost of one
// planning pass.
Plan replan_rules(const AutoPlan & facts, const RunConfig & cfg, const PlanRequest & req, const MeasuredKnobs & knobs);

// Calibrate `ap` (planned by probe_and_plan for `cfg` and `req`) with `run` as each candidate, and
// leave the decided plan in it (AutoPlan::calibrated, calibration). `on_step` (nullable) hears each
// candidate as it starts, as AutoPlanOptions::on_step does. A plan with nothing pending runs nothing.
const CalibrationResult &
calibrate_plan(AutoPlan & ap,
               const RunConfig & cfg,
               const PlanRequest & req,
               const RunCandidate & run,
               const std::function<void(const char * step, int index, int count)> & on_step = nullptr);

// The child's side of an isolated device probe, whole: start the backends, probe `device` for the
// model at `model_path`, and return in `line` what the child prints on its standard output for the
// parent to find - the facts marker and the facts. False when there is no such device to probe.
bool probe_device_child(const char * model_path, const std::string & device, std::string & line);

} // namespace bmoe
