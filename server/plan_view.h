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
#include "bmoe/model_profile.h"
#include "bmoe/plan.h"

#include <string>

namespace bmoe::server {

// summary, predicted, memory, devices, devices_none and devices_none_text, as one object to merge
// into the /api/plan body.
json plan_view(const HardwareProfile & hw, const ModelProfile & model, const Plan & plan);

// One decision on the wire: the planner's fields (`brief` falls back to the reason where a decision
// has none), a label - its knob's settings label, or for a note the name of its topic - and the
// group it is read in (`set`, `kept`, `notes`).
json decision_json(const Decision & d);

// The words for a stage of the measurement (bmoe/auto_plan.h, k_plan_steps); a stage this does
// not know is worded from its key.
std::string plan_step_label(const std::string & step);

} // namespace bmoe::server
