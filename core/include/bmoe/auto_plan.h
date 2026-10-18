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

#include "bmoe/config.h"
#include "bmoe/hardware_profile.h"
#include "bmoe/model_profile.h"
#include "bmoe/plan.h"

#include <functional>
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
};

// The plan and the two profiles it was made from: a front-end that explains a plan quotes the
// facts (every device's figures, the model's weight groups) beside the decisions. The first
// stage's placement is the plan's own (Plan::placement).
struct AutoPlan {
    HardwareProfile hw;
    ModelProfile model;
    Plan plan;
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

// The child's side of an isolated device probe, whole: start the backends, probe `device` for the
// model at `model_path`, and return in `line` what the child prints on its standard output for the
// parent to find - the facts marker and the facts. False when there is no such device to probe.
bool probe_device_child(const char * model_path, const std::string & device, std::string & line);

} // namespace bmoe
