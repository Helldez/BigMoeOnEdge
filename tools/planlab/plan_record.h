// A plan's inputs as a value: everything `--auto` measured on one machine for one model, kept as
// text, so the same plan can be made again anywhere - with no model file, no device and no drive.
//
// The rules are pure functions over a HardwareProfile, a ModelProfile and the first stage's
// Placement (bmoe/planner.h), so a machine the project does not own is, to the rules, exactly the
// facts its probes reported. A record is those facts. Made on a phone, a rented card or a
// contributor's desktop, it turns that machine into a test that runs on every build:
//
//   - replayed as recorded, it must make the plan it made (a rule change that moves a plan on a
//     machine nobody has at hand shows up as a failing test, not as a report weeks later);
//   - replayed with a knob pinned, it says what the rules would do there with that knob - the
//     question a rented machine was otherwise booted to answer;
//   - replayed with a fact changed (less memory, a slower drive), it says what the same model would
//     get on the machine next to it;
//   - carrying what the run then measured, it sets the cost model's prediction beside the truth.
//
// What a record is NOT is an emulator. It answers "what would the planner decide and predict", and
// the prediction is only as good as the cost model: whether the plan is FAST is still answered by
// running it on the machine. See docs/plan-records.md.
//
// A developer's instrument, not part of the engine: it is built into bmoe-planlab and the
// plan_records test and into nothing a user runs. Pure: no llama.cpp, no I/O, no clock.
#pragma once

#include "bmoe/config.h"
#include "bmoe/hardware_profile.h"
#include "bmoe/model_profile.h"
#include "bmoe/placement.h"
#include "bmoe/plan.h"
#include "bmoe/planner.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bmoe {

// What the run this plan produced then measured. All unset on a record of a plan alone.
struct PlanObserved {
    int generated_tokens = 0;           // 0: no run was recorded
    double seconds_per_token = 0.0;     // decode, wall
    double compute_s_per_token = 0.0;   // of which compute
    double io_s_per_token = 0.0;        // of which reading experts
    double cache_hit_decode_pct = -1.0; // the expert cache over decoded tokens; -1: no cache
    // What the cache really was, against what the plan credited hits at: the budget the run ended
    // with, the bytes resident in it, and the experts the system dropped from it under pressure. A
    // cache the kernel keeps smaller than its budget is most of a missed prediction where it happens.
    double cache_budget_mib = 0.0;
    double cache_resident_mib = 0.0;
    long long cache_discarded = 0;
    int prompt_tokens = 0;
    double prefill_seconds = 0.0;

    bool any() const { return generated_tokens > 0; }
};

struct PlanRecord {
    std::string engine; // bmoe::version() that made it
    // Free text about where it was made, for a reader. Never parsed. Phrase the machine generically.
    std::string note;

    // The configuration the plan started from, as parameter keys and values (bmoe/params.h), only
    // those that were not the default. A path is kept as its file name alone - the rules read
    // whether one was given, never where it is - and free text (a prompt) is not kept at all.
    std::vector<std::pair<std::string, std::string>> config;
    PlanRequest request;

    HardwareProfile hw;
    ModelProfile model;
    std::vector<PlacedAt> placements; // in the order asked; the first is the one at the caller's width

    // What the rules made of it when it was recorded: the regime and every parameter the plan
    // resolved, on one line (resolved_line). The line a replay is compared with.
    std::string resolved;
    double predicted_s_per_token = 0.0;

    PlanObserved observed;
};

// A plan as one comparable line: its regime, then every parameter of the resolved config that is
// not the default, spelled by the parameter table, paths as file names.
std::string resolved_line(const Plan & plan);

// The record of a plan just made. `base` and `request` are what the rules were given.
PlanRecord make_plan_record(const RunConfig & base,
                            const PlanRequest & request,
                            const HardwareProfile & hw,
                            const ModelProfile & model,
                            const std::vector<PlacedAt> & placements,
                            const Plan & plan);

// The record as text, one `key=value` per line, and back. A key the reader does not know is
// reported in `unknown` and otherwise ignored; a key the text does not carry leaves its field at
// the default, so a record outlives the fields added after it. Where a key is repeated the last
// one wins, which is what makes "the same machine with one fact changed" one appended line.
std::string format_plan_record(const PlanRecord & rec);
bool parse_plan_record(const std::string & text,
                       PlanRecord & out,
                       std::string & error,
                       std::vector<std::string> * unknown = nullptr);

// The recorded starting configuration, applied over the defaults. A key or value this build's
// parameter table refuses is named in `problems` and left at its default.
RunConfig record_config(const PlanRecord & rec, std::vector<std::string> & problems);

struct Replay {
    Plan plan;
    // Where the replay stands on less than the record: a placement asked at a width the fitter was
    // never asked at on that machine (moved along its measured slope instead), a context or KV type
    // the placement was not made for. Empty: the plan stands on recorded facts alone.
    std::vector<std::string> caveats;
};

// Plan `cfg` on the record's machine and model. With record_config() and the record's own request
// this is the plan it recorded; with a knob changed and pinned, what the rules do there with it.
Replay replay_plan(const PlanRecord & rec, const RunConfig & cfg, const PlanRequest & request);

} // namespace bmoe
