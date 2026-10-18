// The probe-and-plan sequence (bmoe/auto_plan.h): the adapters of bmoe/probe.h in the one order a
// plan is measured in, then the rules. Composition only - every fact comes from an adapter and
// every decision from plan_run().

#include "bmoe/auto_plan.h"

#include "bmoe/planner.h"
#include "bmoe/probe.h"

#include "llama.h"

#include <algorithm>

namespace bmoe {

AutoPlan probe_and_plan(const RunConfig & cfg, const PlanRequest & req, const AutoPlanOptions & opt) {
    AutoPlan r;
    const char * path = cfg.model_path.c_str();

    register_backends();
    r.hw = probe_hardware(path);
    // Every device beyond the host is asked in a process of its own first. A device is only as sound
    // as its driver, and a driver that ends the process it is used from would otherwise end this plan
    // - seen on a phone whose graphics driver lacks a function a backend calls without checking. What
    // comes back is that device's facts, or the reason it has none; the probes below then leave it
    // alone either way. Where a second process cannot be started the runner is empty and they run
    // here.
    if (opt.isolated_probe_args)
        probe_devices_isolated(r.hw, self_process_runner(opt.isolated_probe_args, opt.isolated_probe_seconds));
    r.model = probe_model(path);
    probe_device_support(r.hw, r.model);
    llama_backend_init();
    probe_bandwidth(r.hw, r.model);
    probe_device_costs(r.hw, r.model);
    // Where the expert reads land, which on a split model is not the file the caller named.
    if (opt.probe_io)
        probe_storage(r.hw, r.model.expert_file.empty() ? path : r.model.expert_file.c_str(),
                      r.model.expert_slice_bytes);
    // What the run must at least hold: the dense set and the cache FLOOR the engine will enforce,
    // which is the larger of one token cycle and the generic guard - not one token cycle alone.
    const uint64_t guard = (uint64_t) MoeStreamConfig::cache_min_mb << 20;
    probe_headroom(r.hw, opt.probe_mem, r.model.dense_bytes + (std::max)(r.model.token_cycle_bytes, guard));
    const Placement placement = probe_placement(path, r.model, r.hw, (uint32_t) cfg.n_ctx, (uint32_t) cfg.n_ubatch);
    r.plan = plan_run(cfg, r.hw, r.model, placement, req);
    return r;
}

bool probe_device_child(const char * model_path, const std::string & device, std::string & line) {
    register_backends();
    llama_backend_init();
    std::string facts;
    if (!probe_one_device(model_path, device, facts)) return false;
    line = std::string(device_facts_marker) + " " + facts;
    return true;
}

} // namespace bmoe
