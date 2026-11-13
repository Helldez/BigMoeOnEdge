// The probe-and-plan sequence (bmoe/auto_plan.h): the adapters of bmoe/probe.h in the one order a
// plan is measured in, then the rules. Composition only - every fact comes from an adapter and
// every decision from plan_run().

#include "bmoe/auto_plan.h"
#include "bmoe/backend_choice.h"

#include "bmoe/planner.h"
#include "bmoe/params.h"
#include "bmoe/probe.h"
#include "bmoe/routing.h"
#include "bmoe/measured_knobs.h"
#include "bmoe/version.h"
#include "../metrics/memory_snapshot.h"

#include "../engine/llama_log.h"

#include "llama.h"

#include <algorithm>
#include <filesystem>

namespace bmoe {

namespace {

// The rules on the facts a probe gathered (probe_and_plan's planning half): plan_run on the first
// stage's placement, and the capacity fitter asked again where the plan narrows the graph. `model`
// is the facts' own, or the same with another record of measurements (replan).
Plan plan_layout(const AutoPlan & facts, const RunConfig & cfg, const PlanRequest & req, const ModelProfile & model) {
    const char * path = cfg.model_path.c_str();
    const HardwareProfile & hw = facts.hw;
    const Placement & placement = facts.placement;
    Plan plan = plan_run(cfg, hw, model, placement, req);
    // The fitter placed layers around compute buffers as wide as the graph it was given. Where the plan
    // narrowed the graph and a device took buffers of it, the room the narrower buffers leave is the
    // fitter's to give (the dense part of every layer before any cache), so it places again at that
    // width and the rules run on what it places. That includes a device the wide buffers left with no
    // layer at all, where placing again is the whole point. The width stays the first plan's decision:
    // it was made against the wide placement, which is what it narrowed. One pass. A plan may also
    // choose a graph wider than the probe's (the whole context, priced from the probe's slope), but
    // only where no device's placement depends on it: a device that computes the graph is bounded at
    // PlannerPolicy::ubatch_max_device, no wider than the probed default, and one short of room at
    // its share of the probed width.
    const uint32_t width = (uint32_t) plan.config.n_ubatch;
    if (!req.is_pinned("ubatch") && placement.fitted && !placement.shared_memory_placement &&
        (placement.n_gpu_layers > 0 || placement.device_compute_per_token > 0) && width > 0 &&
        width < placement.compute_width) {
        RunConfig narrow = cfg;
        narrow.n_ubatch = (int) width;
        const Placement at_width = probe_placement(path, model, hw, (uint32_t) cfg.n_ctx, width, cfg.cache_type_k,
                                                   cfg.cache_type_v, facts.backends, &cfg.spec);
        detail::log_memory_snapshot("auto-probe-narrow");
        if (at_width.fitted) {
            const Decision * first = nullptr;
            for (const Decision & d : plan.decisions)
                if (d.knob == "ubatch") first = &d;
            const Decision kept = first ? *first : Decision{};
            const KnobTrialConfig trial = plan.config.trial;
            plan = plan_run(narrow, hw, model, at_width, req);
            // Narrowed again on the new placement: that decision stands, it is the narrower one. So does
            // a width trial the first plan set, and the narrow width its requests are recorded against:
            // the second sees its narrow width as the full one.
            if (plan.config.n_ubatch == (int) width) {
                for (Decision & d : plan.decisions)
                    if (d.knob == "ubatch" && !kept.knob.empty()) d = kept;
                if ((trial.width_side >= 0 || trial.width_narrow > 0) && plan.config.trial.device_tier < 0) {
                    plan.config.trial.width_side = trial.width_side;
                    plan.config.trial.width_narrow = trial.width_narrow;
                }
            }
        }
    }
    // Every layer on a device of its own and the experts still on the host, behind a device cache: a
    // narrower graph may leave room for the whole model on the device, which serves every expert from
    // device memory and computes a prompt without gathering experts into a cache, so it wins prefill
    // and decode alike. Only a placement can say whether it fits, so the fitter is asked at the width
    // the device's share allows, and the narrower graph is kept only where it places everything there.
    // Measured on a 24 GB card with Qwen3.6-35B Q4_K_M at a 16384 context: 8192 tokens wide, the whole
    // model fit, and decode went from 137-139 to 155-156 tok/s and a 9714-token prefill from 4789 to
    // 5594 tok/s. Where it does not fit whole, the wide graph stands (that trade is not priced).
    else if (!req.is_pinned("ubatch") && placement.fitted && !placement.shared_memory_placement &&
             placement.n_gpu_layers != 0 && !placement.host_expert_layers.empty() &&
             (plan.config.n_ubatch == 0 || (uint32_t) plan.config.n_ubatch >= placement.compute_width)) {
        const uint32_t candidate =
            device_share_width(placement, hw, PlannerPolicy::defaults(), (uint32_t) cfg.graph_width_floor());
        if (candidate > 0 && candidate < placement.compute_width) {
            const Placement at_width = probe_placement(path, model, hw, (uint32_t) cfg.n_ctx, candidate,
                                                       cfg.cache_type_k, cfg.cache_type_v, facts.backends, &cfg.spec);
            detail::log_memory_snapshot("auto-probe-whole");
            if (at_width.fitted && at_width.host_expert_layers.empty() && at_width.n_gpu_layers != 0) {
                RunConfig narrow = cfg;
                narrow.n_ubatch = (int) candidate;
                plan = plan_run(narrow, hw, model, at_width, req);
                const std::string w = std::to_string(candidate);
                for (Decision & d : plan.decisions)
                    if (d.knob == "ubatch") {
                        d.value = w;
                        d.source = Source::Measured;
                        d.brief = w + " tokens per pass: at this width the whole model fits on the device.";
                        d.reason = "a graph as wide as the context left the experts on the host behind a device "
                                   "cache; at " +
                                   w +
                                   " tokens the capacity fitter places every layer and every expert on the devices, "
                                   "which serves every expert from device memory and computes a prompt without "
                                   "gathering experts into a cache. " +
                                   flag_spelling("ubatch") + " sets it by hand";
                    }
            }
        }
    }
    return plan;
}

// Restate a decision the give-way pass below made, in the plan it made it in.
void restate(
    Plan & q, const char * knob, const std::string & value, const std::string & brief, const std::string & why) {
    for (Decision & d : q.decisions)
        if (d.knob == knob) {
            d.value = value;
            d.source = Source::Derived;
            d.brief = brief;
            d.reason = why;
            return;
        }
    q.decisions.push_back(Decision{knob, value, Source::Derived, why, brief});
}

// What gives way to the expert cache. The context checkpoints and the context's KV are held for the
// whole run beside the cache, and where the person set neither, an amount that leaves the cache under
// one token cycle (an eviction per read, no hits) has bought insurance and history with every decoded
// token's speed. Measured on a 12 GB phone: Gemma 4 26B-A4B at the server's 16384 context kept no
// expert cache and no NPU prefill, and decoded at 0.5 s a token. So, in this order, while the cache is
// under its floor: the checkpoints halve, down to one (the end of the last prompt, where a follow-up
// most often diverges), then the context halves, the fitter placing again at each length, down to
// PlannerPolicy::ctx_min_auto. The first plan whose cache clears the floor is the run's; where none
// does, the plan stays as it was, since giving them up bought nothing. A value the person set is
// theirs and never moves (the plan still names the lever).
Plan give_way(
    const AutoPlan & facts, const RunConfig & cfg, const PlanRequest & req, const ModelProfile & model, Plan plan) {
    const uint64_t cycle = plan.token_cycle_bytes;
    if (plan.streaming_declined || !plan.config.moe.enabled || cycle == 0 || plan.cache_budget_bytes >= cycle)
        return plan;
    const auto clears = [&](const Plan & q) { return !q.streaming_declined && q.cache_budget_bytes >= cycle; };
    const auto mib = [](uint64_t b) { return std::to_string(b >> 20); };
    RunConfig base = cfg;
    const int kept = cfg.ctx_checkpoints;
    if (!req.is_pinned("ctx-checkpoints") && cfg.chatml && kept > 1) {
        for (int n = kept / 2; n >= 1; n /= 2) {
            base.ctx_checkpoints = n;
            Plan q = plan_layout(facts, base, req, model);
            if (!clears(q)) continue;
            restate(q, "ctx-checkpoints", std::to_string(n),
                    std::to_string(n) + " context checkpoints, not " + std::to_string(kept) +
                        ": the cache needed the room.",
                    "no count was set, and " + std::to_string(kept) +
                        " checkpoints left the expert cache under one token cycle (" + mib(cycle) +
                        " MiB), which returns no hits; with " + std::to_string(n) + " the cache is " +
                        mib(q.cache_budget_bytes) +
                        " MiB. A follow-up that diverges before the oldest one kept is prefilled from there. " +
                        flag_spelling("ctx-checkpoints") + " sets it by hand");
            return q;
        }
        base.ctx_checkpoints = 1;
    }
    const uint32_t floor = PlannerPolicy::defaults().ctx_min_auto;
    const int given = cfg.n_ctx;
    if (req.is_pinned("ctx-size") || given <= (int) floor) return plan;
    for (uint32_t n_ctx = (uint32_t) given / 2; n_ctx >= floor; n_ctx /= 2) {
        RunConfig shorter = base;
        shorter.n_ctx = (int) n_ctx;
        AutoPlan at = facts;
        at.placement = probe_placement(cfg.model_path.c_str(), model, facts.hw, n_ctx, (uint32_t) shorter.graph_width(),
                                       cfg.cache_type_k, cfg.cache_type_v, facts.backends, &cfg.spec);
        detail::log_memory_snapshot("auto-probe-context");
        if (!at.placement.fitted && facts.placement.fitted) break;
        Plan q = plan_layout(at, shorter, req, model);
        if (!clears(q)) continue;
        const std::string n = std::to_string(n_ctx);
        restate(q, "ctx-size", n, n + " tokens of context: at " + std::to_string(given) + " no expert cache fit.",
                "no context was set, and at " + std::to_string(given) +
                    " tokens its memory left the expert cache under one token cycle (" + mib(cycle) +
                    " MiB), which returns no hits; at " + n + " tokens the cache is " + mib(q.cache_budget_bytes) +
                    " MiB. A longer conversation does not fit. " + flag_spelling("ctx-size") +
                    " sets it by hand, and a context set is never shortened");
        if (shorter.ctx_checkpoints != kept)
            restate(q, "ctx-checkpoints", std::to_string(shorter.ctx_checkpoints),
                    "1 context checkpoint, not " + std::to_string(kept) + ": the cache needed the room.",
                    "no count was set, and fewer checkpoints alone did not leave the expert cache a token cycle, so "
                    "they went down to one before the context was shortened. " +
                        flag_spelling("ctx-checkpoints") + " sets it by hand");
        return q;
    }
    return plan;
}

Plan plan_on_facts(const AutoPlan & facts, const RunConfig & cfg, const PlanRequest & req, const ModelProfile & model) {
    return give_way(facts, cfg, req, model, plan_layout(facts, cfg, req, model));
}

} // namespace

AutoPlan probe_and_plan(const RunConfig & cfg, const PlanRequest & req, const AutoPlanOptions & opt) {
    AutoPlan r;
    const char * path = cfg.model_path.c_str();
    int step = 0;
    const auto starting = [&] {
        if (opt.on_step && step < k_plan_step_count) opt.on_step(k_plan_steps[step], step, k_plan_step_count);
        ++step;
    };

    detail::log_memory_snapshot("auto-probe-begin");
    starting(); // devices
    register_backends();
    // The probes load the model too; asked for, their DEBUG lines are wanted as much as the run's.
    detail::set_llama_log_debug(cfg.llama_debug_log);
    r.hw = probe_hardware(path);
    detail::log_memory_snapshot("auto-hardware");
    // Every device beyond the host is asked in a process of its own first. A device is only as sound
    // as its driver, and a driver that ends the process it is used from would otherwise end this plan
    // - seen on a phone whose graphics driver lacks a function a backend calls without checking. What
    // comes back is that device's facts, or the reason it has none; the probes below then leave it
    // alone either way. Where a second process cannot be started the runner is empty and they run
    // here.
    if (opt.isolated_probe_args)
        probe_devices_isolated(r.hw, self_process_runner(opt.isolated_probe_args, opt.isolated_probe_seconds));
    starting(); // model
    r.model = probe_model(path);
    // The head of a file of its own is a file like the model: its size is what placing it costs.
    if (!cfg.spec.mtp_head.empty()) {
        std::error_code ec;
        const auto head_size = std::filesystem::file_size(cfg.spec.mtp_head, ec);
        if (!ec) r.model.mtp_head_bytes = (uint64_t) head_size;
    }
    // The projector the same: held whole for the run, so its size is what it costs where it goes.
    if (!cfg.mmproj.empty()) {
        std::error_code ec;
        const auto proj_size = std::filesystem::file_size(cfg.mmproj, ec);
        if (!ec) r.model.projector_bytes = (uint64_t) proj_size;
    }
    // The model's own routing, from an earlier run the front-end kept: what its cache will hit.
    if (opt.routing)
        attach_routing(r.model, *opt.routing,
                       cfg.n_expert_used > 0 ? (uint32_t) cfg.n_expert_used : r.model.n_expert_used,
                       cfg.moe.cache_freq_half_life);
    // What an earlier load measured about the lossless knobs, when it was this machine and engine.
    if (opt.knobs)
        if (const MeasuredKnobs * k = find_measured_knobs(*opt.knobs, hardware_fingerprint(r.hw), version()))
            r.model.knobs = *k;
    probe_device_support(r.hw, r.model);
    detail::log_memory_snapshot("auto-device-support");
    llama_backend_init();
    starting(); // compute
    probe_bandwidth(r.hw, r.model);
    detail::log_memory_snapshot("auto-bandwidth");
    // Only route-ahead reads this fact, and route-ahead arms only when the caller sets it, so a plan
    // without it does not pay the timing or the working set.
    if (cfg.moe.route_ahead > 0) probe_gate_gemv(r.hw, r.model);
    probe_device_costs(r.hw, r.model);
    detail::log_memory_snapshot("auto-device-costs");
    starting(); // storage
    // Where the expert reads land, which on a split model is not the file the caller named.
    if (opt.probe_io)
        probe_storage(r.hw, r.model.expert_file.empty() ? path : r.model.expert_file.c_str(),
                      r.model.expert_slice_bytes);
    detail::log_memory_snapshot("auto-storage");
    // What the run must at least hold: the dense set and the cache FLOOR the engine will enforce,
    // which is the larger of one token cycle and the generic guard - not one token cycle alone.
    starting(); // memory
    const uint64_t guard = (uint64_t) MoeStreamConfig::cache_min_mb << 20;
    probe_headroom(r.hw, opt.probe_mem, r.model.dense_bytes + (std::max)(r.model.token_cycle_bytes, guard));
    detail::log_memory_snapshot("auto-headroom");
    starting(); // placement
    // The fitter places on the backends the plan will choose, one per card.
    const BackendChoice backends = req.is_pinned("device-list") || req.is_pinned("devices")
                                       ? BackendChoice{}
                                       : choose_backends(r.hw, (double) PlannerPolicy::defaults().min_backend_win);
    r.placement = probe_placement(path, r.model, r.hw, (uint32_t) cfg.n_ctx, (uint32_t) cfg.graph_width(),
                                  cfg.cache_type_k, cfg.cache_type_v, backends.keep, &cfg.spec);
    r.backends = backends.keep;
    detail::log_memory_snapshot("auto-probe-end");
    starting(); // rules
    r.plan = plan_on_facts(r, cfg, req, r.model);
    if (opt.calibrate) calibrate_plan(r, cfg, req, opt.calibrate, opt.on_step);
    return r;
}

Plan replan_rules(const AutoPlan & facts, const RunConfig & cfg, const PlanRequest & req, const MeasuredKnobs & knobs) {
    ModelProfile model = facts.model;
    model.knobs = knobs;
    return plan_run(cfg, facts.hw, model, facts.placement, req);
}

Plan replan(const AutoPlan & facts, const RunConfig & cfg, const PlanRequest & req, const MeasuredKnobs & knobs) {
    ModelProfile model = facts.model;
    model.knobs = knobs;
    return plan_on_facts(facts, cfg, req, model);
}

const CalibrationResult & calibrate_plan(AutoPlan & ap,
                                         const RunConfig & cfg,
                                         const PlanRequest & req,
                                         const RunCandidate & run,
                                         const std::function<void(const char * step, int index, int count)> & on_step) {
    if (!calibration_due(ap.plan, ap.model.knobs, [&](const MeasuredKnobs & k) { return replan(ap, cfg, req, k); }))
        return ap.calibration;
    const RunCandidate reported = [&](const RunConfig & c, const CalibrationRun & cr) {
        if (on_step) on_step(k_calibrate_step, cr.index, cr.runs_max);
        return run(c, cr);
    };
    ap.calibration =
        calibrate(ap.plan, ap.model.knobs, [&](const MeasuredKnobs & k) { return replan(ap, cfg, req, k); }, reported);
    ap.calibrated = true;
    ap.plan = ap.calibration.plan;
    ap.model.knobs = ap.calibration.knobs;
    return ap.calibration;
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
