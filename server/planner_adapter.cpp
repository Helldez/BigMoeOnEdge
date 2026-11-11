#include "planner_adapter.h"

#include "plan_view.h"

#include "bmoe/params.h"

#if defined(BMOE_HAVE_PLANNER)
#include "bmoe/auto_plan.h"

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

#if defined(BMOE_HAVE_PLANNER)
static PlanOutcome outcome_of(const PlanBasis & b);
#endif

PlanOutcome make_plan(const RunConfig & cfg,
                      const std::vector<std::string> & pinned,
                      bool isolated_probes,
                      const PlanStepFn & on_step,
                      const RoutingTrace * routing,
                      const std::vector<MeasuredKnobs> * knobs,
                      bool measure_knobs) {
    PlanOutcome o;
#if !defined(BMOE_HAVE_PLANNER)
    (void) routing;
    (void) knobs;
    (void) measure_knobs;
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
    req.measure_knobs = measure_knobs;

    // The probe order is core's (probe_and_plan), the one bmoe-cli --auto runs. What the server
    // decides: the storage is measured, the headroom only ever estimated - never the intrusive
    // active probe from a server - and every device beyond the host is asked in a process of its
    // own, because a driver that ends the process it is used from would otherwise end the server
    // with the UI waiting on it, and this server is what an app on a phone runs behind.
    AutoPlanOptions opt;
    opt.probe_io = true;
    opt.probe_mem = false;
    opt.routing = routing;
    opt.knobs = knobs;
    if (isolated_probes) {
        const std::string model = cfg.model_path;
        opt.isolated_probe_args = [model](const std::string & dev) {
            return std::vector<std::string>{"--probe-device", dev, "-m", model};
        };
    }
    if (on_step)
        opt.on_step = [&on_step](const char * step, int index, int count) {
            on_step(step, plan_step_label(step), index, count);
        };
    auto basis = std::make_shared<PlanBasis>();
    basis->facts = probe_and_plan(cfg, req, opt);
    basis->cfg = cfg;
    basis->req = req;
    o = outcome_of(*basis);
    o.basis = std::move(basis);
    return o;
#endif
}

#if defined(BMOE_HAVE_PLANNER)
// A plan as the API returns it: what it changes, through the parameter table, and why.
static PlanOutcome outcome_of(const PlanBasis & b) {
    PlanOutcome o;
    const RunConfig & cfg = b.cfg;
    const HardwareProfile & hw = b.facts.hw;
    const Plan & plan = b.facts.plan;

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
        o.decisions.push_back(decision_json(d));

    o.ok = true;
    o.held_bytes = plan.allocation.ledger.held;
    o.hardware = hardware_fingerprint(hw);
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
    // What a person reads first: the outcome, the memory, the devices (plan_view.h).
    o.body.update(plan_view(hw, plan));
    return o;
}
#endif

bool PlanOutcome::side_pending() const {
    return ok && basis && bmoe::side_pending(basis->facts.plan);
}

CalibrationOutcome calibrate_plan_outcome(const PlanOutcome & plan,
                                          const MeasuredKnobs & start,
                                          const RunCandidate & run,
                                          const PlanStepFn & on_step) {
    CalibrationOutcome c;
#if !defined(BMOE_HAVE_PLANNER)
    (void) plan;
    (void) start;
    (void) run;
    (void) on_step;
    return c;
#else
    if (!plan.side_pending()) return c;
    auto basis = std::make_shared<PlanBasis>(*plan.basis);
    // The record as it stands now: a served answer may have timed a side since the plan was made, and
    // the plan on it may already ask for less.
    basis->facts.plan = replan(basis->facts, basis->cfg, basis->req, start);
    basis->facts.model.knobs = start;
    std::function<void(const char *, int, int)> step;
    if (on_step)
        step = [&on_step](const char * key, int index, int count) { on_step(key, plan_step_label(key), index, count); };
    const CalibrationResult & r = calibrate_plan(basis->facts, basis->cfg, basis->req, run, step);
    c.ran = true;
    c.aborted = r.aborted;
    c.complete = !bmoe::side_pending(basis->facts.plan);
    c.knobs = basis->facts.model.knobs;
    c.plan = outcome_of(*basis);
    c.plan.hardware = plan.hardware;
    c.plan.basis = std::move(basis);
    return c;
#endif
}

} // namespace bmoe::server
