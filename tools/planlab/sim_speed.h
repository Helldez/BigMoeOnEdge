// A decode estimate for any plan, including the ones the cost model does not price.
//
// The planner's cost model predicts a token's time where the experts stream (Allocation::
// seconds_per_token); where llama.cpp's placement is the whole plan (streaming declined: a card holds
// the layers, maybe a device cache of experts) it predicts nothing, and that is most of what a rented
// card runs. A simulated machine needs a number for both, so this file adds one, for the tool only:
// the parts a decoded token goes through, one after the other, each at a rate a probe measured -
//
//   - the dense part of the layers on each card, and of the layers left on the host;
//   - the experts that live on a card;
//   - a device cache of experts: its hits on the card, its misses uploaded and computed there (or a
//     share of them computed on the host, with a host hop per layer);
//   - the experts the host computes from RAM, or, where they stream, the cost model's own price of
//     them (RAM cache hits and flash reads);
//   - two crossings for every card layer whose experts stay on the host.
//
// It is a sum of measured rates and nothing else: no overlap, no launch overhead, no thermal state.
// Its error against measured runs is what docs/hardware-simulation.md reports, and the simulator
// prints that range beside it. Nothing in the engine reads it.
#pragma once

#include "bmoe/hardware_profile.h"
#include "bmoe/model_profile.h"
#include "bmoe/placement.h"
#include "bmoe/plan.h"

#include <string>
#include <utility>
#include <vector>

namespace bmoe {

struct DecodeEstimate {
    double seconds = 0.0;                              // per decoded token; 0: a rate it needs is unmeasured
    std::vector<std::pair<std::string, double>> parts; // what makes it up, in seconds
    double device_cache_hit = -1.0;                    // the share of the host's experts a device cache serves
    bool hit_from_routing = false;                     // that share from the model's own routing
    std::string why;                                   // why there is no estimate
};

// `placement` is the fitter's answer the plan was made on (the one at the plan's own width).
DecodeEstimate
estimate_decode(const Plan & plan, const HardwareProfile & hw, const ModelProfile & model, const Placement & placement);

} // namespace bmoe
