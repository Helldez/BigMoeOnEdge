#include "planner_adapter.h"

#include "bmoe/params.h"

#if defined(BMOE_HAVE_PLANNER)
#include "bmoe/planner.h"
#include "bmoe/probe.h"

#include "llama.h"

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
    // Where the expert reads land, which on a split model is not the file the caller named.
    probe_storage(hw, mp.expert_file.empty() ? path : mp.expert_file.c_str(), mp.expert_slice_bytes);
    const uint64_t guard = (uint64_t) MoeStreamConfig::cache_min_mb << 20;
    const uint64_t cache_floor = mp.token_cycle_bytes > guard ? mp.token_cycle_bytes : guard;
    probe_headroom(hw, HeadroomEstimate::Auto, false, mp.dense_bytes + cache_floor);
    const Placement placement = probe_placement(path, mp, hw, (uint32_t) cfg.n_ctx, (uint32_t) cfg.n_ubatch);
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
