// Load-time calibration's loop (bmoe/calibrate.h) with fake ports: a planner that names a side from
// the record it is given, and candidates that answer from a table. What is checked is the loop's
// policy: it ends, it runs the sides in the order the planner names them, it opens nothing the planner
// does not ask for, a candidate that cannot open is a losing side, the record keeps what it started
// with, every later run replays the first one's tokens against the other side's range, and a stopped
// run ends it. The real rules on the same loop are in planner_test; the engine's candidate in moe_gates.

#include "bmoe/calibrate.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace bmoe;

namespace {

int failures = 0;

void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// A plan naming one side of one decision. The candidate's config tells the sides apart the way a
// real plan's does: by what it places (a device cache, or a narrower graph).
Plan tier_side(int side) {
    Plan p;
    p.config.trial.device_tier = side;
    p.config.moe.device_cache_mb = side == 1 ? 4096 : 0;
    p.config.n_gpu_layers = side == 1 ? 99 : 30;
    return p;
}
Plan width_side(int side, int narrow) {
    Plan p;
    p.config.trial.width_side = side;
    p.config.trial.width_narrow = narrow;
    p.config.n_ubatch = side == 1 ? 0 : narrow;
    return p;
}
Plan decided(int cache_mb, int ubatch = 0) {
    Plan p;
    p.config.moe.device_cache_mb = cache_mb;
    p.config.n_ubatch = ubatch;
    return p;
}

// The device tier as the planner decides it: the cache first, then the fitter's placement, the faster
// kept; a side that could not open loses.
Plan tier_rules(const MeasuredKnobs & k) {
    if (k.device_cache_unviable) return decided(0);
    if (k.no_device_cache_unviable) return decided(4096);
    if (k.device_cache_tps <= 0.0) return tier_side(1);
    if (k.no_device_cache_tps <= 0.0) return tier_side(0);
    return decided(k.no_device_cache_tps > k.device_cache_tps * 1.05 ? 0 : 4096);
}

struct Runs {
    std::vector<RunConfig> configs;
    std::vector<CalibrationRun> asked;
};

CandidateTiming timed(double tps, std::vector<int32_t> tokens = {11, 12, 13}) {
    CandidateTiming t;
    t.created = true;
    t.tokens_per_second = tps;
    t.step_lo = 0.9 / tps;
    t.step_hi = 1.1 / tps;
    t.windows = 7;
    t.tokens = std::move(tokens);
    return t;
}

RunCandidate table(Runs & runs, std::function<CandidateTiming(const RunConfig &)> f) {
    return [&runs, f](const RunConfig & c, const CalibrationRun & r) {
        runs.configs.push_back(c);
        runs.asked.push_back(r);
        return f(c);
    };
}

} // namespace

int main() {
    // ── both sides of the device tier: two runs, in the planner's order, the faster served ──
    {
        Runs runs;
        MeasuredKnobs start;
        start.threads = 6; // a verdict of another kind, already kept
        start.overlap = 1;
        Plan first = tier_side(1);
        first.config.trial.threads = {4, 8}; // the served load's knob trial: never a candidate's
        first.config.trial.overlap = true;
        const CalibrationResult r = calibrate(first, start, tier_rules, table(runs, [](const RunConfig & c) {
                                                  return timed(c.moe.device_cache_mb > 0 ? 17.5 : 9.9, {5, 6, 7, 8});
                                              }));
        check(r.complete && !r.aborted && r.runs == 2 && runs.configs.size() == 2, "tier: two runs, then decided");
        check(runs.configs.size() == 2 && runs.configs[0].moe.device_cache_mb > 0 &&
                  runs.configs[1].moe.device_cache_mb == 0,
              "tier: the cache side first, as the planner named it, then the fitter's");
        check(runs.configs.size() == 2 && runs.configs[0].trial.threads.empty() && !runs.configs[0].trial.overlap &&
                  runs.configs[0].trial.device_tier == -1 && runs.configs[1].trial.width_side == -1,
              "tier: a candidate carries no knob trial and names no side");
        check(r.knobs.device_cache_tps == 17.5 && r.knobs.no_device_cache_tps == 9.9,
              "tier: both rates in the record, with the session's mapping");
        check(r.knobs.threads == 6 && r.knobs.overlap == 1, "tier: what the record started with is kept");
        check(r.plan.config.moe.device_cache_mb == 4096 && !side_pending(r.plan), "tier: the cache is served");
        check(runs.asked.size() == 2 && runs.asked[0].replay.empty() && runs.asked[0].reference_lo == 0.0 &&
                  runs.asked[1].replay == std::vector<int32_t>({5, 6, 7, 8}),
              "tier: the first run samples, the second replays its tokens");
        check(runs.asked.size() == 2 && runs.asked[1].reference_lo > 0.0 && runs.asked[1].reference_lo == 0.9 / 17.5 &&
                  runs.asked[1].reference_hi == 1.1 / 17.5,
              "tier: the second run is set against the first side's range");
        check(runs.asked.size() == 2 && runs.asked[0].index == 0 && runs.asked[1].index == 1 &&
                  runs.asked[1].runs_max == CalibrationPolicy{}.runs_max,
              "tier: each run is told its place");
    }

    // ── nothing pending: nothing opened ──
    {
        Runs runs;
        const CalibrationResult r = calibrate(decided(0), MeasuredKnobs{}, tier_rules,
                                              table(runs, [](const RunConfig &) { return timed(1.0); }));
        check(r.complete && r.runs == 0 && runs.configs.empty(), "nothing pending: no candidate");
    }

    // ── the planner's ceiling: once the first side is past what the other could reach, it stops asking ──
    {
        Runs runs;
        const auto ceiling = [](const MeasuredKnobs & k) {
            if (k.device_cache_tps <= 0.0) return tier_side(1);
            return k.device_cache_tps > 20.0 ? decided(4096) : tier_side(0);
        };
        const CalibrationResult r = calibrate(tier_side(1), MeasuredKnobs{}, ceiling,
                                              table(runs, [](const RunConfig &) { return timed(40.0); }));
        check(r.complete && r.runs == 1 && r.knobs.no_device_cache_tps == 0.0,
              "ceiling: the side that cannot win is never opened");
    }

    // ── a candidate that cannot be opened: that side loses, and the planner stops asking ──
    {
        Runs runs;
        const CalibrationResult r =
            calibrate(tier_side(1), MeasuredKnobs{}, tier_rules, table(runs, [](const RunConfig & c) {
                          if (c.moe.device_cache_mb > 0) return timed(12.0);
                          CandidateTiming t;
                          t.error = "out of device memory";
                          return t;
                      }));
        check(r.complete && r.runs == 2 && r.knobs.no_device_cache_unviable && !r.knobs.device_cache_unviable &&
                  r.knobs.device_cache_tps == 12.0 && r.plan.config.moe.device_cache_mb == 4096,
              "unviable: the fitter's placement failed to open, the cache is served");
        Runs first_fails;
        const CalibrationResult f = calibrate(tier_side(1), MeasuredKnobs{}, tier_rules,
                                              table(first_fails, [](const RunConfig &) { return CandidateTiming{}; }));
        check(f.complete && f.runs == 1 && f.knobs.device_cache_unviable && f.plan.config.moe.device_cache_mb == 0,
              "unviable: the cache failed to open, the fitter's placement is served without timing it");
    }

    // ── a side that failed to open before: lost on every plan, tried again by a calibration within
    //    the retry budget, cleared by a success, kept by a failure or when it is not reached ──
    {
        MeasuredKnobs start;
        start.device_cache_tps = 12.0;
        start.no_device_cache_unviable = 1;
        check(!side_pending(tier_rules(start)) && calibration_due(tier_rules(start), start, tier_rules),
              "retry: the failed side loses the plan, and a calibration is due to try it again");
        // A planner that would not ask for the side now (a person pinned the decision): not due, so a
        // server does not unload a model for a calibration that would open nothing.
        const auto pinned = [](const MeasuredKnobs &) { return decided(4096); };
        check(!calibration_due(decided(4096), start, pinned), "retry: a decision the plan no longer asks is not due");
        Runs runs;
        const CalibrationResult ok =
            calibrate(tier_rules(start), start, tier_rules,
                      table(runs, [](const RunConfig & c) { return timed(c.moe.device_cache_mb > 0 ? 12.0 : 20.0); }));
        check(ok.runs == 1 && runs.configs.size() == 1 && runs.configs[0].moe.device_cache_mb == 0 &&
                  ok.knobs.no_device_cache_unviable == 0 && ok.knobs.no_device_cache_tps == 20.0 &&
                  ok.plan.config.moe.device_cache_mb == 0 && ok.complete,
              "retry: the side is opened again, opens, and wins");
        Runs again;
        const CalibrationResult bad = calibrate(tier_rules(start), start, tier_rules,
                                                table(again, [](const RunConfig &) { return CandidateTiming{}; }));
        check(bad.runs == 1 && bad.knobs.no_device_cache_unviable == 2 && bad.plan.config.moe.device_cache_mb == 4096,
              "retry: a second failure counts on top of the first, and the other side is served");
        check(!calibration_due(bad.plan, bad.knobs, tier_rules), "retry: past the budget it is not due again");
        Runs none;
        const CalibrationResult spent =
            calibrate(bad.plan, bad.knobs, tier_rules, table(none, [](const RunConfig &) { return timed(1.0); }));
        check(spent.runs == 0 && spent.knobs.no_device_cache_unviable == 2,
              "retry: past the budget, nothing is opened");
        Runs stopped;
        const CalibrationResult aborted =
            calibrate(tier_rules(start), start, tier_rules, table(stopped, [](const RunConfig &) {
                          CandidateTiming t;
                          t.created = true;
                          t.aborted = true;
                          return t;
                      }));
        check(aborted.aborted && aborted.knobs.no_device_cache_unviable == 1 &&
                  aborted.plan.config.moe.device_cache_mb == 4096 && !side_pending(aborted.plan),
              "retry: stopped before a verdict, the side keeps its count and still loses the plan");
    }

    // ── the device tier, then the graph width at the winner: a configuration already timed is not
    //    opened again, and a width side is set against the other width only ──
    {
        Runs runs;
        const int narrow = 2048;
        const auto rules = [&](const MeasuredKnobs & k) {
            Plan p = tier_rules(k);
            if (side_pending(p)) return p;
            const int cache = p.config.moe.device_cache_mb;
            if (k.wide_decode_tps <= 0.0) {
                Plan w = width_side(1, narrow);
                w.config.moe.device_cache_mb = cache;
                w.config.n_gpu_layers = 99; // the configuration the cache side ran
                return w;
            }
            if (k.narrow_decode_tps <= 0.0) {
                Plan w = width_side(0, narrow);
                w.config.moe.device_cache_mb = cache;
                w.config.n_gpu_layers = 99;
                return w;
            }
            return decided(cache, k.narrow_decode_tps > k.wide_decode_tps * 1.05 ? narrow : 0);
        };
        const CalibrationResult r =
            calibrate(tier_side(1), MeasuredKnobs{}, rules, table(runs, [&](const RunConfig & c) {
                          if (c.moe.device_cache_mb == 0) return timed(9.0);
                          return timed(c.n_ubatch == narrow ? 30.0 : 20.0);
                      }));
        check(r.complete && r.runs == 3 && runs.configs.size() == 3,
              "tier then width: three candidates, the cache's full width timed once for both decisions");
        check(r.knobs.device_cache_tps == 20.0 && r.knobs.wide_decode_tps == 20.0 &&
                  r.knobs.narrow_decode_tps == 30.0 && r.knobs.width_narrow == narrow,
              "tier then width: the one run answers the tier's cache side and the width's full side");
        check(r.plan.config.moe.device_cache_mb == 4096 && r.plan.config.n_ubatch == narrow,
              "tier then width: the cache at the narrow width is served");
        check(runs.asked.size() == 3 && runs.asked[2].reference_lo == 0.9 / 20.0,
              "tier then width: the narrow side is set against the full width's run");
    }

    // ── a plan that keeps asking: the hard stop, and a run that gives the plan nothing new ──
    {
        Runs runs;
        int asked = 0;
        const auto restless = [&asked](const MeasuredKnobs &) {
            Plan p = tier_side(1);
            p.config.moe.device_cache_mb = 1000 + ++asked; // a new configuration every time
            return p;
        };
        Plan first = tier_side(1);
        const CalibrationResult r =
            calibrate(first, MeasuredKnobs{}, restless,
                      table(runs, [](const RunConfig & c) { return timed(10.0 + c.moe.device_cache_mb * 1e-3); }));
        check(!r.complete && r.runs == CalibrationPolicy{}.runs_max && (int) runs.configs.size() == r.runs,
              "restless plan: stopped at the most runs, the side left to the served load");
        Runs same;
        const CalibrationResult s = calibrate(
            width_side(1, 512), MeasuredKnobs{}, [](const MeasuredKnobs &) { return width_side(1, 512); },
            table(same, [](const RunConfig &) { return timed(10.0); }));
        check(!s.complete && s.runs == 1 && s.knobs.wide_decode_tps == 10.0,
              "a plan asking again for a side already in the record: stopped after one run");
        Runs mute;
        const CalibrationResult m =
            calibrate(tier_side(1), MeasuredKnobs{}, tier_rules, table(mute, [](const RunConfig &) {
                          CandidateTiming t;
                          t.created = true; // opened, but decoded too little to time
                          return t;
                      }));
        check(!m.complete && m.runs == 1 && m.knobs.empty(), "a candidate with no rate: stopped, nothing recorded");
    }

    // ── a stopped run ends the calibration and records nothing for it ──
    {
        Runs runs;
        const CalibrationResult r =
            calibrate(tier_side(1), MeasuredKnobs{}, tier_rules, table(runs, [](const RunConfig & c) {
                          if (c.moe.device_cache_mb > 0) return timed(12.0);
                          CandidateTiming t;
                          t.created = true;
                          t.aborted = true;
                          return t;
                      }));
        check(r.aborted && !r.complete && r.runs == 2 && r.knobs.device_cache_tps == 12.0 &&
                  r.knobs.no_device_cache_tps == 0.0 && !r.knobs.no_device_cache_unviable &&
                  r.plan.config.trial.device_tier == 0,
              "aborted: the first side kept, the second left pending for the served load");
    }

    // ── the width's record: a rate at a new narrow width starts it afresh ──
    {
        MeasuredKnobs k;
        k.width_narrow = 1024;
        k.wide_decode_tps = 5.0;
        k.narrow_unviable = true;
        KnobTrialConfig t;
        t.width_side = 0;
        t.width_narrow = 2048;
        record_side_rate(t, 7.0, k);
        check(k.width_narrow == 2048 && k.wide_decode_tps == 0.0 && k.narrow_decode_tps == 7.0 && !k.narrow_unviable,
              "record: another narrow width resets the width's record");
        KnobTrialConfig both;
        both.device_tier = 1;
        both.width_side = 1;
        both.width_narrow = 2048;
        record_side_rate(both, 9.0, k);
        check(k.device_cache_tps == 9.0 && k.wide_decode_tps == 0.0, "record: one decision per run, the tier first");
    }

    // What a served load may open as the plan names it (servable): nothing pending, or the width's
    // wide side, which is the graph the plan's other loads already ran with. An unproven placement
    // (either side of the device tier) or the narrow graph is left to a calibration.
    {
        Plan p;
        check(servable(p), "servable: nothing pending");
        p.config.trial.width_side = 1;
        check(servable(p), "servable: the graph at the plan's own width, timed as it serves");
        p.config.trial.width_side = 0;
        check(!servable(p), "servable: the narrow graph is not served before a calibration has run it");
        p.config.trial.width_side = -1;
        p.config.trial.device_tier = 0;
        check(!servable(p), "servable: the fitter's placement unproven is not served");
        p.config.trial.device_tier = 1;
        check(!servable(p), "servable: a device cache unproven is not served either");
    }

    // ── the device cache's CPU share, the third decision: the tier first, then the share on its winner ──
    {
        const auto share_side = [](int side) {
            Plan p = decided(4096);
            p.config.trial.cpu_share_side = side;
            p.config.moe.device_cache_cpu_share = side == 1 ? 0.7f : 0.0f;
            return p;
        };
        // The tier as above; on the cache, the share: every miss uploaded first, then the share.
        const Replan rules = [&](const MeasuredKnobs & k) {
            const Plan tier = tier_rules(k);
            if (tier.config.trial.device_tier >= 0 || tier.config.moe.device_cache_mb == 0) return tier;
            if (k.cpu_share_unviable) return decided(4096);
            const double without = k.no_cpu_share_tps > 0.0 ? k.no_cpu_share_tps : k.device_cache_tps;
            if (without <= 0.0) return share_side(0);
            if (k.cpu_share_tps <= 0.0) return share_side(1);
            Plan d = decided(4096);
            if (k.cpu_share_tps > without * 1.1) d.config.moe.device_cache_cpu_share = 0.7f;
            return d;
        };
        Runs runs;
        const CalibrationResult r =
            calibrate(rules(MeasuredKnobs{}), MeasuredKnobs{}, rules, table(runs, [](const RunConfig & c) {
                          if (c.moe.device_cache_mb == 0) return timed(9.9);
                          return timed(c.moe.device_cache_cpu_share > 0.0f ? 21.2 : 18.5);
                      }));
        check(r.complete && r.runs == 3 && r.knobs.device_cache_tps == 18.5 && r.knobs.no_device_cache_tps == 9.9 &&
                  r.knobs.cpu_share_tps == 21.2,
              "share: the tier's two sides, then the share on the cache, each timed once");
        check(r.plan.config.moe.device_cache_mb == 4096 && r.plan.config.moe.device_cache_cpu_share > 0.0f &&
                  !side_pending(r.plan),
              "share: the faster share served, nothing left pending");
        check(runs.configs.size() == 3 && runs.configs[2].moe.device_cache_cpu_share > 0.0f &&
                  runs.configs[2].trial.cpu_share_side == -1,
              "share: its candidate carries the share and names no side");
        Runs lost_runs;
        const CalibrationResult lost =
            calibrate(rules(MeasuredKnobs{}), MeasuredKnobs{}, rules, table(lost_runs, [](const RunConfig & c) {
                          if (c.moe.device_cache_mb == 0) return timed(9.9);
                          return timed(c.moe.device_cache_cpu_share > 0.0f ? 68.0 : 114.0);
                      }));
        check(lost.complete && lost.plan.config.moe.device_cache_cpu_share == 0.0f,
              "share: measured slower (a cache that never fills), every miss uploaded");
        check(!servable(share_side(1)) && servable(share_side(0)),
              "servable: a CPU share is served only once measured; the cache with every miss uploaded is the "
              "cache every earlier load ran");
    }

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall calibration checks passed\n");
    return 0;
}
