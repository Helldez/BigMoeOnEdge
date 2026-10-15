#include "planner_adapter.h"

#include "bmoe/params.h"

#if defined(BMOE_HAVE_PLANNER)
#include "bmoe/planner.h"
#include "bmoe/probe.h"

#include "llama.h"

#include <cstdio>
#endif

#include <vector>

namespace bmoe::server {

bool planner_available() {
#if defined(BMOE_HAVE_PLANNER)
    return true;
#else
    return false;
#endif
}

// What a device probe may take before the parent gives up on it: a backend's first allocation
// on a slow driver, and nothing a plan should wait longer for. The CLI's figure.
static constexpr double k_device_probe_seconds = 120.0;

int run_device_probe_child(const std::string & model_path, const std::string & device) {
#if !defined(BMOE_HAVE_PLANNER)
    (void) model_path;
    (void) device;
    return 3;
#else
    register_backends();
    llama_backend_init();
    std::string facts;
    if (!probe_one_device(model_path.c_str(), device, facts)) {
        std::fprintf(stderr, "bmoe-server: no such device to probe: %s\n", device.c_str());
        return 3;
    }
    std::printf("%s %s\n", device_facts_marker, facts.c_str());
    std::fflush(stdout);
    return 0;
#endif
}

PlanOutcome make_plan(const RunConfig & cfg, const std::vector<std::string> & pinned, bool isolated_probes) {
    PlanOutcome o;
#if !defined(BMOE_HAVE_PLANNER)
    (void) cfg;
    (void) pinned;
    (void) isolated_probes;
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
    // Every device beyond the host is asked in a process of its own first, as bmoe-cli --auto does:
    // a driver that ends the process it is used from would otherwise end the server with the UI
    // waiting on it, and this server is what an app on a phone runs behind.
    if (isolated_probes) {
        const std::string model = cfg.model_path;
        probe_devices_isolated(hw, self_process_runner(
                                       [model](const std::string & dev) {
                                           return std::vector<std::string>{"--probe-device", dev, "-m", model};
                                       },
                                       k_device_probe_seconds));
    }
    const ModelProfile mp = probe_model(path);
    probe_device_support(hw, mp);
    llama_backend_init();
    probe_bandwidth(hw, mp);
    probe_device_costs(hw, mp);
    // Where the expert reads land, which on a split model is not the file the caller named.
    probe_storage(hw, mp.expert_file.empty() ? path : mp.expert_file.c_str(), mp.expert_slice_bytes);
    const uint64_t guard = (uint64_t) MoeStreamConfig::cache_min_mb << 20;
    const uint64_t cache_floor = mp.token_cycle_bytes > guard ? mp.token_cycle_bytes : guard;
    probe_headroom(hw, false, mp.dense_bytes + cache_floor);
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
