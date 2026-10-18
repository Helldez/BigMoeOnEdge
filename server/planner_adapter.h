// The hardware planner, as the server offers it.
//
// The planner is a library the engine may or may not carry (see server/CMakeLists.txt). With it,
// this makes the plan with the same call as `bmoe-cli --auto` (bmoe/auto_plan.h) and turns the Plan
// into what the API returns: its decisions, and the parameter values it would set, expressed through the
// parameter table so they land in the same settings layers as everything else. Without it, every
// call says so.
#pragma once

#include "json_util.h"

#include "bmoe/config.h"

#include <map>
#include <string>
#include <vector>

namespace bmoe::server {

struct PlanOutcome {
    bool ok = false;
    std::string error;                         // why there is no plan (no planner, no model)
    json body;                                 // the /api/plan object
    std::map<std::string, std::string> values; // parameters the plan changes, by key
    json decisions = json::array();
};

bool planner_available();

// Plan `cfg` (its model must be set) on this machine. `pinned` are the operator's keys, which the
// planner leaves alone. Probes the machine: a second or two of storage reads and a bandwidth run.
// With `isolated_probes`, each compute device beyond the host is first asked in a process of its
// own (this executable again, with --probe-device), so a driver that ends the process it is used
// from marks the device unusable instead of ending the server; where a second process cannot be
// started the probes run here.
PlanOutcome make_plan(const RunConfig & cfg, const std::vector<std::string> & pinned, bool isolated_probes = true);

// The child's side of an isolated probe: probe `device` for `model_path`, print its facts after
// the marker, and return the process's exit code. 3 when there is no such device or no planner.
int run_device_probe_child(const std::string & model_path, const std::string & device);

} // namespace bmoe::server
