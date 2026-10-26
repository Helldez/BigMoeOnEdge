// The plan as a person reads it: what /api/plan adds to the planner's decisions so a UI can draw
// the Plan screen (docs/screens.md) without knowing a device, a system or a knob.
//
// Pure: it reads the two profiles and the plan, and writes JSON. Every list is as long as the
// machine makes it, every name is the plan's own, and every word a UI shows comes from here, next
// to a code (`status`, `uses`, `phase`, `step`) that a UI reads only to pick a colour. So the
// words live once, for every UI, and a stage or a role added here reaches all of them unchanged.
#pragma once

#include "json_util.h"

#include "bmoe/hardware_profile.h"
#include "bmoe/plan.h"

#include <cstdint>
#include <string>

namespace bmoe::server {

// Which held plan answers a read with a model loaded (`GET /api/plan` without `measure`). The two
// plans the server holds are each named by the model they are for and their place in the order
// measured (0: none held): the one the load applied, and the last one measured.
struct PlanRead {
    std::string model; // the loaded model
    bool want_cached = false;
    std::string load_model;
    uint64_t load_seq = 0;
    std::string last_model;
    uint64_t last_seq = 0;
};
// `Runs`: the plan the model was loaded with, while nothing was measured after that load, with what
// the model has done since beside it. `Last`: the last plan measured, for the same model, when the
// caller asked for a held one - a measurement made by hand with the model resident, which Apply
// applies, shown with the model's figures but not compared to them. `None`: nothing held for this
// model; the read measures, or says so.
enum class HeldPlan { None, Runs, Last };
HeldPlan held_plan(const PlanRead & r);

// summary, predicted (decode as priced, the hit rate it credited, the nothing-resident bound),
// memory, devices, devices_none and devices_none_text, as one object to merge into /api/plan.
json plan_view(const HardwareProfile & hw, const Plan & plan);

// A decision's value as a person reads it: a choice by its settings label, a size in GiB or MiB,
// a number with its unit, a planner word ("experts-stream") as words. `value` stays the raw one.
std::string value_text(const Decision & d);

// One decision on the wire: the planner's fields (`brief` falls back to the reason where a decision
// has none), a label - its knob's settings label, or for a note the name of its topic - and the
// group it is read in (`set`, `kept`, `notes`).
json decision_json(const Decision & d);

// The words for a stage of the measurement (bmoe/auto_plan.h, k_plan_steps); a stage this does
// not know is worded from its key.
std::string plan_step_label(const std::string & step);

} // namespace bmoe::server
