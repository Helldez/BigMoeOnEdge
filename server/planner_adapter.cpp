#include "planner_adapter.h"

#include "bmoe/params.h"

#if defined(BMOE_HAVE_PLANNER)
#include "bmoe/planner.h"
#include "bmoe/probe.h"

#include "llama.h"

#include <sstream>
#endif

namespace bmoe::server {

bool planner_available() {
#if defined(BMOE_HAVE_PLANNER)
    return true;
#else
    return false;
#endif
}

PlanOutcome make_plan(const RunConfig & cfg, const std::vector<std::string> & pinned) {
    PlanOutcome o;
#if !defined(BMOE_HAVE_PLANNER)
    (void) cfg;
    (void) pinned;
    o.error = "this build does not include the automatic hardware planner";
    return o;
#else
    if (cfg.model_path.empty()) {
        o.error = "choose a model first: a plan is for one model on this machine";
        return o;
    }
    PlanRequest req;
    req.pinned = pinned;

    // The probe order of bmoe-cli --auto, and for its reasons: register backends before anything
    // looks at devices; quiet measurements before dirty ones; the headroom estimate (never the
    // intrusive active probe from a server) last.
    const char * path = cfg.model_path.c_str();
    register_backends();
    HardwareProfile hw = probe_hardware(path);
    const ModelProfile mp = probe_model(path);
    probe_device_support(hw, mp);
    llama_backend_init();
    probe_bandwidth(hw, mp);
    probe_device_costs(hw, mp);
    probe_storage(hw, path, mp.expert_slice_bytes);
    const uint64_t guard = (uint64_t) MoeStreamConfig::cache_min_mb << 20;
    const uint64_t cache_floor = mp.token_cycle_bytes > guard ? mp.token_cycle_bytes : guard;
    probe_headroom(hw, false, mp.dense_bytes + cache_floor);
    const Placement placement = probe_placement(path, mp, hw, (uint32_t) cfg.n_ctx);
    const Plan plan = plan_run(cfg, hw, mp, placement, req);

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
    // A flag the plan emits that this build's table cannot read is a decision the settings cannot
    // hold (a placement knob, say). Listed rather than dropped, so the gap is visible.
    json not_applicable = json::array();
    json args = json::array();
    {
        std::istringstream in(plan.to_flags());
        std::vector<std::string> toks;
        for (std::string t; in >> t;)
            toks.push_back(t);
        RunConfig scratch = cfg;
        for (size_t i = 0; i < toks.size(); ++i) {
            args.push_back(toks[i]);
            if (toks[i].rfind("--", 0) != 0) continue;
            const FlagResult r =
                apply_flag(scratch, toks[i].c_str(), i + 1 < toks.size() ? toks[i + 1].c_str() : nullptr);
            if (!r.matched) {
                not_applicable.push_back(toks[i]);
            } else if (r.consumed_value) {
                args.push_back(toks[++i]);
            }
        }
    }
    for (const Decision & d : plan.decisions)
        o.decisions.push_back(
            {{"knob", d.knob}, {"value", d.value}, {"source", source_name(d.source)}, {"reason", d.reason}});

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
                  {"explain", plan.explain()}};
    return o;
#endif
}

} // namespace bmoe::server
