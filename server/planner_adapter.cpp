#include "planner_adapter.h"

#include "bmoe/params.h"

#if defined(BMOE_HAVE_PLANNER)
#include "bmoe/auto_plan.h"

#include <algorithm>
#include <cstdio>
#endif

#include <vector>

namespace bmoe::server {

#if defined(BMOE_HAVE_PLANNER)
namespace {

// What a plan says at a glance, in the words a person reads first. Built from the regime and the
// allocation alone, so it reads the same on every machine; the names in it are the plan's own.
std::string summary_sentence(const Plan & plan) {
    if (plan.streaming_declined)
        return plan.decline_reason.empty() ? "Streaming declined." : "Streaming declined: " + plan.decline_reason;
    switch (plan.regime) {
    case Regime::ExpertsStream: {
        const uint64_t cache = plan.allocation.cache_bytes;
        if (cache == 0) return "Experts stream from flash, without a cache.";
        char gb[32];
        std::snprintf(gb, sizeof gb, "%.1f GiB", (double) cache / (double) (1ull << 30));
        return std::string("Experts stream from flash, with a ") + gb + " cache.";
    }
    case Regime::Fits:
        return "The whole model fits in memory: nothing streams.";
    case Regime::DenseOversized:
        return "Not even the dense weights fit in memory.";
    case Regime::NotMoe:
        return "Not a mixture-of-experts model: there is nothing to stream.";
    case Regime::Unknown:
        break;
    }
    return "The model could not be read.";
}

// A device's memory is its own only when that was measured; an unknown is shared, as the planner
// reads it.
bool own_memory(const ComputeDevice & d) {
    return !d.is_cpu && d.shares_host_memory == Tri::No;
}

bool has_layers(const Plan & plan, const ComputeDevice & d) {
    const Placement & pl = plan.placement;
    if (!pl.fitted || pl.shared_memory_placement || pl.device_bytes == 0 || !own_memory(d)) return false;
    return std::find(pl.projected_devices.begin(), pl.projected_devices.end(), d.name) != pl.projected_devices.end();
}

// Every device the plan saw, with what it does in this run and why, from the facts the rules read.
// The role is a closed set a UI colours by (`uses`, `status`); the words are for the person.
json devices_json(const HardwareProfile & hw, const Plan & plan) {
    const std::string & armed = plan.config.prefill.device;
    json out = json::array();
    for (const ComputeDevice & d : hw.devices) {
        json uses = json::array();
        std::string status = "used", why;
        const double prompt_ratio = d.is_cpu ? 1.0
                                    : (hw.host_wide_matmul_gibs > 0 && d.wide_matmul_gibs > 0)
                                        ? d.wide_matmul_gibs / hw.host_wide_matmul_gibs
                                        : 0.0;
        const double token_ratio = d.is_cpu ? 1.0
                                   : (hw.host_bandwidth_gibs > 0 && d.memory_bandwidth_gibs > 0)
                                       ? d.memory_bandwidth_gibs / hw.host_bandwidth_gibs
                                       : 0.0;
        if (d.is_cpu) {
            if (armed.empty()) uses.push_back("prompt");
            uses.push_back("tokens");
            why = "The host: every rate is measured against it.";
        } else if (d.usable == Tri::No) {
            status = "unusable";
            why = d.unusable_reason.empty() ? "Its probe did not come back, so nothing uses it." : d.unusable_reason;
        } else if (d.is_host_helper) {
            status = "unused";
            why = "A library the host calls into: nothing is placed on it.";
        } else {
            if (has_layers(plan, d)) uses.push_back("layers");
            if (d.name == armed) uses.push_back("prompt");
            if (!uses.empty()) {
                why = d.name == armed ? "Faster than the host on a prompt, with the same results."
                                      : "Holds layers in memory of its own.";
            } else if (d.identity_ok == Tri::No || d.wide_identity_ok == Tri::No) {
                status = "refused";
                why = "Its results did not match the host's, so it is never used.";
            } else if (prompt_ratio == 0) {
                status = "unused";
                why = "Not measured at prompt width, so the plan does not arm it.";
            } else {
                status = "unused";
                why = armed.empty() ? "The prompt stays on the host: the prefill decision says why."
                                    : armed + " is the one the plan arms for the prompt.";
            }
        }
        if (uses.empty() && status == "used") status = "unused";
        out.push_back({{"name", d.name},
                       {"description", d.description},
                       {"cpu", d.is_cpu},
                       {"own_memory", own_memory(d)},
                       {"memory_total", d.memory_total},
                       {"prompt_ratio", prompt_ratio},
                       {"token_ratio", token_ratio},
                       {"uses", uses},
                       {"status", status},
                       {"why", why}});
    }
    return out;
}

// Why the list holds no device beyond the host, when it does not: three different answers that
// must not read as one (hardware_profile.h, backends_looked).
std::string devices_none(const HardwareProfile & hw) {
    for (const ComputeDevice & d : hw.devices)
        if (!d.is_cpu) return "";
    if (!hw.backends_looked) return "not_probed";
    return hw.backends_linked + hw.backends_loaded > 1 ? "machine" : "build";
}

// Where the run's memory goes: the ledger the planner charged, one pool per memory that is
// separate. The host's pool is the ledger; a device with memory of its own that holds layers is
// a second pool, from the placement.
json memory_json(const HardwareProfile & hw, const Plan & plan) {
    const Ledger & l = plan.allocation.ledger;
    json rows = json::array();
    for (const LedgerRow & r : l.rows)
        rows.push_back({{"name", r.name}, {"bytes", r.bytes}, {"locked", r.locked}, {"in_turn", r.in_turn}});
    json pools = json::array();
    pools.push_back({{"name", "host"},
                     {"cap_bytes", l.holdable_cap},
                     {"lockable_cap_bytes", l.lockable_cap},
                     {"held_bytes", l.held},
                     {"locked_bytes", l.locked},
                     {"rows", rows}});
    const Placement & pl = plan.placement;
    for (const ComputeDevice & d : hw.devices) {
        if (!has_layers(plan, d)) continue;
        // The placement sums its device figures over every device it placed on; a per-device split
        // is what the fitter projects for compute only, so the weights are shared out by it.
        const uint64_t compute = pl.device_compute_of(d.name);
        const uint64_t fixed = pl.device_compute_bytes + pl.device_context_bytes;
        const uint64_t weights = pl.device_bytes > fixed ? pl.device_bytes - fixed : 0;
        const size_t n = pl.projected_devices.empty() ? 1 : pl.projected_devices.size();
        json drows = json::array();
        if (weights)
            drows.push_back({{"name", "layers"}, {"bytes", weights / n}, {"locked", false}, {"in_turn", false}});
        if (compute)
            drows.push_back({{"name", "context and compute"},
                             {"bytes", compute + pl.device_context_bytes / n},
                             {"locked", false},
                             {"in_turn", false}});
        pools.push_back({{"name", d.name},
                         {"cap_bytes", d.memory_total},
                         {"lockable_cap_bytes", 0},
                         {"held_bytes", weights / n + compute + pl.device_context_bytes / n},
                         {"locked_bytes", 0},
                         {"rows", drows}});
    }
    return json{{"pools", pools}};
}

// Which phase runs where: the lanes a person asks about first.
json lanes_json(const HardwareProfile & hw, const Plan & plan) {
    std::string host = "CPU";
    for (const ComputeDevice & d : hw.devices)
        if (d.is_cpu) host = d.name;
    json tokens = json::array({host});
    for (const ComputeDevice & d : hw.devices)
        if (has_layers(plan, d)) tokens.push_back(d.name);
    const std::string & armed = plan.config.prefill.device;
    return json::array({json{{"phase", "prompt"}, {"on", json::array({armed.empty() ? host : armed})}},
                        json{{"phase", "tokens"}, {"on", tokens}}});
}

// The three groups a person reads a plan in, by who decided and what: a setting the plan decided
// from a fact (whether or not it differs from the value it had), a setting it left to the caller or
// could not measure, and the notes that are not settings at all (the regime, the ledger, a cost).
const char * decision_group(const Decision & d, bool is_param) {
    if (!is_param) return "notes";
    if (d.source == Source::Operator || d.source == Source::Unprobed) return "kept";
    return "set";
}

} // namespace
#endif

bool planner_available() {
#if defined(BMOE_HAVE_PLANNER)
    return true;
#else
    return false;
#endif
}

int run_device_probe_child(const std::string & model_path, const std::string & device) {
#if !defined(BMOE_HAVE_PLANNER)
    (void) model_path;
    (void) device;
    return 3;
#else
    std::string line;
    if (!probe_device_child(model_path.c_str(), device, line)) {
        std::fprintf(stderr, "bmoe-server: no such device to probe: %s\n", device.c_str());
        return 3;
    }
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    return 0;
#endif
}

PlanOutcome make_plan(const RunConfig & cfg,
                      const std::vector<std::string> & pinned,
                      bool isolated_probes,
                      const PlanStepFn & on_step) {
    PlanOutcome o;
#if !defined(BMOE_HAVE_PLANNER)
    (void) cfg;
    (void) pinned;
    (void) isolated_probes;
    (void) on_step;
    o.error = "this build does not include the automatic hardware planner";
    return o;
#else
    if (cfg.model_path.empty()) {
        o.error = "choose a model first: a plan is for one model on this machine";
        return o;
    }
    PlanRequest req;
    req.pinned = pinned;

    // The probe order is core's (probe_and_plan), the one bmoe-cli --auto runs. What the server
    // decides: the storage is measured, the headroom only ever estimated - never the intrusive
    // active probe from a server - and every device beyond the host is asked in a process of its
    // own, because a driver that ends the process it is used from would otherwise end the server
    // with the UI waiting on it, and this server is what an app on a phone runs behind.
    AutoPlanOptions opt;
    opt.probe_io = true;
    opt.probe_mem = false;
    if (isolated_probes) {
        const std::string model = cfg.model_path;
        opt.isolated_probe_args = [model](const std::string & dev) {
            return std::vector<std::string>{"--probe-device", dev, "-m", model};
        };
    }
    opt.on_step = on_step;
    const AutoPlan planned = probe_and_plan(cfg, req, opt);
    const HardwareProfile & hw = planned.hw;
    const Plan & plan = planned.plan;

    // What the plan changes, read through the parameter table: every parameter whose value in the
    // plan's config differs from the config it was given.
    json values = json::object();
    for (const ParamDesc & d : params()) {
        const std::string v = d.get(plan.config);
        if (v != d.get(cfg)) {
            o.values[d.key] = v;
            values[d.key] = param_string_to_value(d, v);
        }
    }
    // The arguments that reproduce the plan, as the plan itself renders them from the parameter
    // table. Nothing is parsed back here: a decision the table cannot hold is named below instead.
    json not_applicable = json::array();
    json args = json::array();
    for (const std::string & a : plan.reproduce_args())
        args.push_back(a);
    // Tensor-placement patterns are a list the parameter table has no type for: named, not dropped.
    // Only when the table cannot carry them: with a `tensor-overrides` row they are ordinary values.
    if (!find_param("tensor-overrides"))
        for (const std::string & p : plan.config.buft_overrides)
            not_applicable.push_back("tensor override " + p);
    // Each decision carries the settings label of its knob, where the knob is a parameter, and the
    // group it is read in, so a UI shows words and order without knowing any knob.
    for (const Decision & d : plan.decisions) {
        const ParamDesc * p = find_param(d.knob);
        o.decisions.push_back({{"knob", d.knob},
                               {"label", p ? p->label : ""},
                               {"group", decision_group(d, p != nullptr)},
                               {"value", d.value},
                               {"source", source_name(d.source)},
                               {"reason", d.reason}});
    }

    // What the plan expects of decode, priced as bmoe-cli --auto prints it after a run: so a client
    // can set the measured figures of a load beside it.
    json predicted = json::object();
    if (plan.allocation.seconds_per_token > 0) predicted["s_per_token"] = plan.allocation.seconds_per_token;
    if (plan.allocation.cache_bytes > 0 && planned.model.expert_bytes > 0)
        predicted["cache_hit_pct"] = 100.0 * (double) plan.allocation.cache_bytes / (double) planned.model.expert_bytes;

    o.ok = true;
    o.body = json{{"available", true},
                  {"model", cfg.model_path},
                  {"machine", hw.label},
                  {"regime", regime_name(plan.regime)},
                  {"streaming_declined", plan.streaming_declined},
                  {"decline_reason", plan.decline_reason},
                  {"decisions", o.decisions},
                  {"values", values},
                  {"args", args},
                  {"not_applicable", not_applicable},
                  {"summary", {{"sentence", summary_sentence(plan)}, {"lanes", lanes_json(hw, plan)}}},
                  {"predicted", predicted},
                  {"memory", memory_json(hw, plan)},
                  {"devices", devices_json(hw, plan)},
                  {"devices_none", devices_none(hw)},
                  {"explain", plan.explain()}};
    return o;
#endif
}

} // namespace bmoe::server
