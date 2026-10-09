// The hardware planner, as the server offers it.
//
// The planner is a library the engine may or may not carry (see server/CMakeLists.txt). With it,
// this runs the same probes in the same order as `bmoe-cli --auto` and turns the Plan into what
// the API returns: its decisions, and the parameter values it would set, expressed through the
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
PlanOutcome make_plan(const RunConfig & cfg, const std::vector<std::string> & pinned);

} // namespace bmoe::server
