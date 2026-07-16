// The cache governor is pure policy, so its device is a function: "faults iff the budget exceeds
// what I concede". That makes the properties that actually matter testable without a phone —
// that it converges just under an unobservable concession and stops, that it keeps cutting for as
// long as the device keeps taking, that it never cuts below the layer it must stage — none of which
// a byte-identity gate can see, because the governor changes what is resident and never what is
// computed.

#include "bmoe/cache_governor.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace bmoe;

namespace {

int failures = 0;

void check(bool ok, const std::string & what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

constexpr size_t MiB = 1024ull * 1024ull;

CacheGovernorParams params(size_t user_cap, size_t initial, size_t min_cap = 64 * MiB) {
    CacheGovernorParams p;
    p.user_cap = user_cap;
    p.initial = initial;
    p.min_cap = min_cap;
    return p;
}

// A calm token: cache fully resident, no faults.
CacheSignals calm(size_t floor = 0) {
    CacheSignals s;
    s.resident_frac = 1.0;
    s.majflt = 0;
    s.floor = floor;
    return s;
}

// A token during reclaim: the kernel has taken part of the cache.
CacheSignals reclaiming(double frac = 0.5, size_t floor = 0) {
    CacheSignals s;
    s.resident_frac = frac;
    s.majflt = 500;
    s.floor = floor;
    return s;
}

void test_starts_at_initial() {
    CacheGovernor g(params(2000 * MiB, 2000 * MiB));
    check(g.cap() == 2000 * MiB, "starts at the configured budget");
    check(g.ceiling() == CacheGovernor::no_ceiling, "no ceiling is known before any reclaim");
    check(g.cuts() == 0, "no cuts before any pressure");

    CacheGovernor clamped(params(500 * MiB, 2000 * MiB));
    check(clamped.cap() == 500 * MiB, "the initial budget is clamped to the user cap");
}

void test_residency_triggers_a_cut() {
    CacheGovernor g(params(2000 * MiB, 2000 * MiB));
    const CacheGovernor::Decision d = g.on_token(reclaiming());
    check(d.changed, "a residency dip resizes");
    check(d.cap == (size_t) (2000.0 * MiB * 0.7), "the cut is multiplicative");
    check(g.ceiling() == 2000 * MiB, "the budget that provoked reclaim becomes the ceiling");
    check(g.cuts() == 1, "the cut is counted");
}

void test_unmeasured_residency_is_not_pressure() {
    CacheGovernor g(params(2000 * MiB, 2000 * MiB));
    CacheSignals s;
    s.resident_frac = -1.0; // sampler throttled, or a platform that cannot report
    s.majflt = 0;
    check(!g.on_token(s).changed, "a missing sample is not evidence of pressure");
    check(g.cap() == 2000 * MiB, "an unmeasured token leaves the budget alone");
}

void test_fault_reflex_needs_a_baseline() {
    CacheGovernor g(params(2000 * MiB, 2000 * MiB));
    // A storm on the very first token has nothing to be extreme relative to: with no baseline the
    // reflex must stay quiet and let the residency sample decide.
    CacheSignals storm;
    storm.resident_frac = -1.0;
    storm.majflt = 10000;
    check(!g.on_token(storm).changed, "the fault reflex does not fire before a baseline exists");

    // Learn a calm baseline, then storm.
    CacheGovernor g2(params(2000 * MiB, 2000 * MiB));
    for (int i = 0; i < 8; ++i) {
        CacheSignals s = calm();
        s.majflt = 10;
        s.resident_frac = 1.0;
        g2.on_token(s);
    }
    CacheSignals s = storm;
    check(g2.on_token(s).changed, "a fault storm well above the learned baseline cuts");
}

void test_small_fault_counts_are_noise() {
    CacheGovernor g(params(2000 * MiB, 2000 * MiB));
    for (int i = 0; i < 8; ++i) {
        CacheSignals s = calm();
        s.majflt = 0;
        g.on_token(s);
    }
    // Baseline is ~0: without an absolute margin, ratio*0 would make a single fault a war.
    CacheSignals s;
    s.resident_frac = -1.0;
    s.majflt = 5;
    check(!g.on_token(s).changed, "a handful of faults against a zero baseline is not a war");
}

void test_cooldown_collapses_a_burst() {
    CacheGovernor g(params(2000 * MiB, 2000 * MiB));
    g.on_token(reclaiming());
    const size_t after_first = g.cap();
    for (int i = 0; i < 3; ++i)
        g.on_token(reclaiming()); // still reclaiming, but the previous cut has not landed yet
    check(g.cap() == after_first, "a burst of pressure inside the cooldown cuts once");
    check(g.cuts() == 1, "one cut for one burst");
}

// Regression, measured on gpt-oss-120b (docs/bench-data/2026-07-15-pressure/): flooring the loop at
// one TOKEN's working set (1815 MiB there) pinned the budget one cut below the ceiling, inside a war
// the reflex could see and not act on. The floor is mechanical — one LAYER — and pressure outranks
// it: a budget the device refuses is not worth defending at any hit rate.
void test_cuts_below_a_token_working_set_when_the_device_refuses_it() {
    const size_t layer = 50 * MiB;   // the mechanical floor: the layer being staged
    const size_t token = 1815 * MiB; // one token's working set — informative, not a floor
    CacheGovernor g(params(2000 * MiB, 2000 * MiB, /*min_cap*/ 16 * MiB));
    for (int i = 0; i < 500; ++i)
        g.on_token(reclaiming(0.2, layer));
    check(g.cap() < token, "sustained reclaim cuts below one token's working set");
    check(g.cap() == layer, "and stops at the mechanical floor, not at zero");
}

void test_floor_is_respected_when_it_is_reachable() {
    const size_t layer = 50 * MiB;
    CacheGovernor g(params(2000 * MiB, 2000 * MiB, /*min_cap*/ 16 * MiB));
    for (int i = 0; i < 500; ++i)
        g.on_token(reclaiming(0.2, layer));
    check(g.cap() >= layer, "never cuts below the layer it must stage");
}

void test_growth_is_additive_and_capped() {
    CacheGovernorParams p = params(2000 * MiB, 500 * MiB);
    p.calm_tokens = 4;
    p.grow_step = 64 * MiB;
    CacheGovernor g(p);

    for (int i = 0; i < 4; ++i)
        g.on_token(calm());
    check(g.cap() == 564 * MiB, "calm grows by exactly one step");

    for (int i = 0; i < 10000; ++i)
        g.on_token(calm());
    check(g.cap() == 2000 * MiB, "sustained calm climbs back to the user cap");
    check(g.cap() <= 2000 * MiB, "growth never exceeds the user cap");
}

void test_growth_retreats_from_a_known_ceiling() {
    CacheGovernorParams p = params(2000 * MiB, 2000 * MiB);
    p.calm_tokens = 4;
    p.probe_after = 1000000; // never probe in this test
    CacheGovernor g(p);
    g.on_token(reclaiming()); // ceiling := 2000, cap := 1400
    for (int i = 0; i < 10000; ++i)
        g.on_token(calm());
    const size_t limit = (size_t) (2000.0 * MiB * 0.9);
    check(g.cap() <= limit, "growth stays below the ceiling it already lost at");
    check(g.cap() > 1400 * MiB, "but it does grow back toward it");
}

void test_probe_forgets_a_stale_ceiling() {
    CacheGovernorParams p = params(2000 * MiB, 2000 * MiB);
    p.calm_tokens = 4;
    p.probe_after = 8;
    CacheGovernor g(p);
    g.on_token(reclaiming());
    check(g.ceiling() == 2000 * MiB, "the ceiling is remembered");
    // A long calm at the retreat line: the device that refused 2000 MiB may not be the device we
    // are on now (an app exited). Re-test rather than stay small forever.
    for (int i = 0; i < 10000; ++i)
        g.on_token(calm());
    check(g.cap() == 2000 * MiB, "after a long calm the governor re-earns the full budget");
}

// The real property: against a device with a hidden concession, the loop must settle just under it
// and STOP — no perpetual sawtooth between war and retreat.
void test_converges_on_a_synthetic_device() {
    const size_t concession = 1200 * MiB; // hidden from the governor
    CacheGovernorParams p = params(3000 * MiB, 3000 * MiB);
    p.calm_tokens = 8;
    p.probe_after = 1000000; // no probing: measure the settled state, not the re-test
    CacheGovernor g(p);

    std::vector<size_t> caps;
    for (int i = 0; i < 4000; ++i) {
        const bool over = g.cap() > concession;
        g.on_token(over ? reclaiming(0.5, 300 * MiB) : calm(300 * MiB));
        if (i >= 3000) caps.push_back(g.cap());
    }
    for (size_t c : caps)
        check(c <= concession, "the settled budget never exceeds what the device concedes");
    size_t lo = caps.front(), hi = caps.front();
    for (size_t c : caps) {
        lo = c < lo ? c : lo;
        hi = c > hi ? c : hi;
    }
    check(hi - lo <= p.grow_step, "the settled budget stops oscillating");
    check(lo > concession / 2, "and it settles near the concession, not far below it");
    // No cuts in the tail means the loop found the edge and stopped fighting for it.
    const long long cuts_at_3000 = g.cuts();
    for (int i = 0; i < 1000; ++i)
        g.on_token(g.cap() > concession ? reclaiming(0.5, 300 * MiB) : calm(300 * MiB));
    check(g.cuts() == cuts_at_3000, "a settled governor stops cutting");
}

void test_degenerate_caps() {
    CacheGovernor g(params(100 * MiB, 100 * MiB, /*min_cap*/ 100 * MiB));
    for (int i = 0; i < 100; ++i)
        g.on_token(reclaiming(0.1, 0));
    check(g.cap() == 100 * MiB, "cap == min_cap leaves nothing to give back");

    // A floor above the user cap: the cache is pathological by configuration, and the governor's
    // job is only to not make it worse by cutting further.
    CacheGovernor g2(params(200 * MiB, 200 * MiB, 16 * MiB));
    for (int i = 0; i < 100; ++i)
        g2.on_token(reclaiming(0.1, /*floor*/ 900 * MiB));
    check(g2.cap() == 200 * MiB, "a floor above the cap pins the budget at the cap");
}

// ── governor v2: attribution, demotion, plateau, entry test ──
// The new signals default to inert values (dense/swap/demand/mem all 0 or -1) so the v1 scenarios
// above exercise pure AIMD unchanged; these drive the new fields to reach the new behaviour.

constexpr size_t GiB = 1024ull * MiB;

// Learn a quiet baseline: a handful of calm, fully-resident, fault-light tokens.
void learn_calm(CacheGovernor & g, int n = 8, uint64_t majflt = 10) {
    for (int i = 0; i < n; ++i) {
        CacheSignals s = calm();
        s.majflt = majflt;
        g.on_token(s);
    }
}

// Regression (R1, device new): an anon/zram war — high faults, swap climbing — while the cache and
// the dense weights are BOTH fully resident. v1 read the resident cache as "calm" and absorbed the
// 3000-fault storm into its baseline, then grew mid-war. v2 must attribute it to the anon war, never
// poison the baseline, and never grow.
void test_anon_war_does_not_poison_baseline_or_grow() {
    CacheGovernorParams p = params(4000 * MiB, 2000 * MiB);
    p.calm_tokens = 4;
    CacheGovernor g(p);
    learn_calm(g);
    const double calm_baseline = g.baseline();

    const size_t cap_before_war = g.cap();
    size_t max_cap = cap_before_war;
    for (int i = 0; i < 200; ++i) {
        CacheSignals s;
        s.resident_frac = 1.0;             // our cache is resident…
        s.dense_resident_frac = 1.0;       // …and so are the dense weights
        s.swap_delta = 40 * (int64_t) MiB; // but the kernel is compressing our anon memory
        s.majflt = 3000;                   // which we re-fault
        s.floor = 50 * MiB;
        g.on_token(s);
        max_cap = g.cap() > max_cap ? g.cap() : max_cap;
    }
    check(g.baseline() < calm_baseline + 50.0, "an anon war never poisons the calm baseline");
    check(max_cap <= cap_before_war, "the governor never grows into an anon war");
    check(g.war() == CacheGovernor::War::anon || g.state() == CacheGovernor::State::off,
          "the war is attributed to anon (or already demoted)");
}

// Regression (R4): the anon war survives cutting — measured, cuts to 470 MiB left it burning. The
// governor must stop cutting and DEMOTE to slot mode, where the churn that fed the war disappears.
void test_anon_war_demotes_when_cuts_do_not_help() {
    CacheGovernorParams p = params(4000 * MiB, 3000 * MiB);
    CacheGovernor g(p);
    learn_calm(g);

    CacheGovernor::Decision d{};
    bool demoted = false;
    for (int i = 0; i < 500 && !demoted; ++i) {
        CacheSignals s;
        s.resident_frac = 1.0;
        s.dense_resident_frac = 1.0;
        s.swap_delta = 60 * (int64_t) MiB;
        s.majflt = 5000;
        s.floor = 50 * MiB;
        d = g.on_token(s);
        if (d.mode == CacheGovernor::Mode::slots) demoted = true;
    }
    check(demoted, "an unrelieved anon war demotes the cache to slot mode");
    check(g.state() == CacheGovernor::State::off, "the governor is now in the off state");
    // And once off it stays quiet — no more budget churn to feed the war.
    for (int i = 0; i < 10; ++i) {
        CacheSignals s;
        s.resident_frac = 1.0;
        s.swap_delta = 60 * (int64_t) MiB;
        s.majflt = 5000;
        check(!g.on_token(s).changed, "a demoted governor stops acting");
    }
}

// A fault load whose cause is demonstrably NOT ours — cache and dense both fully resident, swap
// flat. The governor must not cut for someone else's pressure; it absorbs the load as the new calm.
void test_foreign_faults_do_not_cut() {
    CacheGovernor g(params(2000 * MiB, 2000 * MiB));
    learn_calm(g);
    for (int i = 0; i < 500; ++i) {
        CacheSignals s = calm();
        s.dense_resident_frac = 1.0;
        s.swap_delta = 0;
        s.majflt = 500; // well above the learned baseline, but with no attributable cause
        check(!g.on_token(s).changed, "foreign faults never cut the budget");
    }
    check(g.cuts() == 0, "no cuts for third-party faults");
    check(g.state() == CacheGovernor::State::on, "and no demotion");
}

// The dense weights declining from their own plateau is an early warning — cut before the faults
// pile up, and re-warm the dense set exactly once per episode.
void test_dense_decline_cuts_early_and_rewarms_once() {
    CacheGovernorParams p = params(2000 * MiB, 2000 * MiB);
    CacheGovernor g(p);
    // Establish a dense plateau near 0.98 over several calm tokens.
    for (int i = 0; i < 8; ++i) {
        CacheSignals s = calm();
        s.majflt = 10;
        s.dense_resident_frac = 0.98;
        g.on_token(s);
    }
    int rewarms = 0;
    bool cut = false;
    for (int i = 0; i < 10; ++i) {
        CacheSignals s = calm();
        s.majflt = 100;
        s.dense_resident_frac = 0.70; // a hard drop below plateau*0.85
        s.floor = 50 * MiB;
        CacheGovernor::Decision d = g.on_token(s);
        if (d.changed) cut = true;
        if (d.rewarm_dense) ++rewarms;
    }
    check(cut, "a dense-residency collapse triggers a cut");
    check(rewarms == 1, "the dense set is re-warmed exactly once per war episode");
    check(g.war() == CacheGovernor::War::dense, "the war is attributed to the dense weights");
}

// Feasibility: a token demands more than the device will ever concede (its whole free room, above
// the reserve, plus the current cache, is still short). No budget can earn hits → demote at once.
void test_infeasible_demand_demotes_immediately() {
    CacheGovernorParams p = params(4000 * MiB, 1000 * MiB);
    p.floor_headroom = 1536 * MiB;
    CacheGovernor g(p);
    CacheSignals s = calm();
    s.token_demand = 1815 * MiB;  // one token routes this much
    s.mem_available = 2000 * MiB; // room above the 1536 reserve is only 464 MiB
    CacheGovernor::Decision d = g.on_token(s);
    check(d.mode == CacheGovernor::Mode::slots, "a demand the device cannot concede demotes to slots");
    check(g.state() == CacheGovernor::State::off, "and enters the off state");
}

// Off → on: once the device concedes room for a demand-sized cache with margin, and it holds for a
// few probes, promote back to the LRU. A single lucky probe must not.
void test_rearm_promotes_when_room_returns() {
    CacheGovernorParams p = params(4000 * MiB, 1000 * MiB);
    p.floor_headroom = 1536 * MiB;
    p.probe_every = 2;
    p.rearm_persist = 3;
    CacheGovernor g(p);
    // Demote first (infeasible).
    {
        CacheSignals s = calm();
        s.token_demand = 1815 * MiB;
        s.mem_available = 2000 * MiB;
        g.on_token(s);
    }
    check(g.state() == CacheGovernor::State::off, "demoted");

    // A single good probe amid bad ones must not promote.
    auto good = [&] {
        CacheSignals s = calm();
        s.token_demand = 1000 * MiB;
        s.mem_available = 4 * GiB; // room 4096-1536 = 2560 > 1000*1.25
        return g.on_token(s);
    };
    auto bad = [&] {
        CacheSignals s = calm();
        s.token_demand = 1000 * MiB;
        s.mem_available = 1600 * MiB; // room 64 < demand
        return g.on_token(s);
    };
    for (int i = 0; i < 4; ++i) {
        good();
        bad();
    }
    check(g.state() == CacheGovernor::State::off, "an intermittent good probe does not re-arm");

    // Sustained room re-arms.
    CacheGovernor::Decision d{};
    for (int i = 0; i < 20 && g.state() == CacheGovernor::State::off; ++i)
        d = good();
    check(g.state() == CacheGovernor::State::on, "sustained conceded room promotes back to the LRU");
    check(d.mode == CacheGovernor::Mode::lru, "the promotion asks for LRU mode");
}

// The utility bound with NO configured cap (user_cap = 0): growth must stop where more budget stops
// buying hits, not run up to the physical expert-set size.
void test_plateau_stops_growth_without_a_configured_cap() {
    CacheGovernorParams p;
    p.user_cap = 0;          // unbounded by configuration
    p.phys_cap = 4000 * MiB; // only the physical set bounds it from above
    p.initial = 200 * MiB;
    p.min_cap = 64 * MiB;
    p.calm_tokens = 4;
    p.grow_step = 64 * MiB;
    p.plateau_confirm = 2;
    CacheGovernor g(p);

    const double SAT = 1000.0 * MiB; // hit rate saturates once the budget reaches here
    long long hits = 0, lookups = 0;
    for (int i = 0; i < 4000; ++i) {
        CacheSignals s = calm();
        const double hr = std::min(1.0, (double) g.cap() / SAT);
        lookups += 200;
        hits += (long long) (200.0 * hr);
        s.cache_hits = hits;
        s.cache_lookups = lookups;
        g.on_token(s);
    }
    check(g.cap() >= 900 * MiB, "growth climbs to where hits still improve");
    check(g.cap() <= 1300 * MiB, "and stops at the hit-rate plateau, not the physical size");
    check(g.cap() < 2000 * MiB, "well below the 4000 MiB expert set");
}

// gpt-oss in a fault war (device CSV 2026-07-16): the fault baseline is poisoned by the cold-start
// storm so the reflex never fires, and MemAvailable reads high so feasibility looks fine — yet the
// budget was cut below one token's demand. The utility demote must catch it on that structural fact.
void test_demotes_when_cut_below_token_demand() {
    CacheGovernorParams p = params(4000 * MiB, 4000 * MiB, /*min_cap*/ 16 * MiB);
    CacheGovernor g(p);
    CacheGovernor::Decision d{};
    bool demoted = false;
    for (int i = 0; i < 200 && !demoted; ++i) {
        CacheSignals s = reclaiming(0.5, 50 * MiB); // cache war → the governor cuts
        s.token_demand = 1815 * MiB;                // but one token needs more than any budget it keeps
        d = g.on_token(s);
        if (d.mode == CacheGovernor::Mode::slots) demoted = true;
    }
    check(demoted, "cutting the budget below one token's demand demotes to slots");
    check(g.state() == CacheGovernor::State::off, "and enters the off state");
}

// A model with no expert reuse: the budget is comfortably above one token's demand, the cache is
// resident, there is no fault war — but the hit rate sits near zero because routing has no locality.
// The cache is pure churn; the windowed-hit-rate demote must retire it.
void test_demotes_on_near_zero_hit_rate() {
    CacheGovernorParams p = params(4000 * MiB, 4000 * MiB);
    p.demote_window = 4;
    CacheGovernor g(p);
    long long hits = 0, lookups = 0;
    CacheGovernor::Decision d{};
    bool demoted = false;
    for (int i = 0; i < 100 && !demoted; ++i) {
        CacheSignals s = calm();
        s.token_demand = 1000 * MiB; // below the 4000 cap, so the below-demand trigger stays silent
        lookups += 100;
        hits += 2; // 2% hit rate: the model has no reuse the cache can exploit
        s.cache_hits = hits;
        s.cache_lookups = lookups;
        d = g.on_token(s);
        if (d.mode == CacheGovernor::Mode::slots) demoted = true;
    }
    check(demoted, "a sustained near-zero hit rate demotes even with budget above demand");
    check(g.state() == CacheGovernor::State::off, "and enters the off state");
}

// gpt-oss's harder case (device CSV): budget above demand AND a middling hit rate (~15%), so neither
// the below-demand nor the near-zero-hit trigger fires — but a sustained thousands-of-faults/token
// war rages (the dense working set churning). The fault-load demote must catch it.
void test_demotes_on_sustained_fault_war() {
    CacheGovernorParams p = params(4000 * MiB, 4000 * MiB);
    p.demote_window = 4;
    CacheGovernor g(p);
    long long hits = 0, lookups = 0;
    CacheGovernor::Decision d{};
    bool demoted = false;
    for (int i = 0; i < 100 && !demoted; ++i) {
        CacheSignals s;
        s.resident_frac = 1.0;
        s.token_demand = 1000 * MiB; // below the 4000 cap → below-demand stays silent
        s.majflt = 12000;            // thousands of faults/token: a memory war
        lookups += 100;
        hits += 15; // 15% hit — above the near-zero floor, so only the fault load can catch this
        s.cache_hits = hits;
        s.cache_lookups = lookups;
        d = g.on_token(s);
        if (d.mode == CacheGovernor::Mode::slots) demoted = true;
    }
    check(demoted, "a sustained heavy fault war demotes even at a middling hit rate");
    check(g.state() == CacheGovernor::State::off, "and enters the off state");
}

// A healthy booster (Qwen-like): budget above demand, climbing hit rate. It must NOT be demoted by
// the utility triggers — the whole point is that gov2 keeps the cache where it earns its keep.
void test_healthy_cache_is_not_demoted() {
    CacheGovernorParams p = params(4000 * MiB, 3000 * MiB);
    p.demote_window = 4;
    CacheGovernor g(p);
    long long hits = 0, lookups = 0;
    for (int i = 0; i < 300; ++i) {
        CacheSignals s = calm();
        s.token_demand = 1000 * MiB; // well below the budget
        lookups += 100;
        hits += 45; // 45% hit rate — the cache is clearly earning
        s.cache_hits = hits;
        s.cache_lookups = lookups;
        check(g.on_token(s).mode != CacheGovernor::Mode::slots, "a cache earning 45% hits is never demoted");
    }
    check(g.state() == CacheGovernor::State::on, "the healthy cache stays on");
}

void test_entry_budget_helper() {
    // Unknown available memory → trust the configured budget.
    check(entry_budget(2000 * MiB, 0, 1536 * MiB, 64 * MiB) == 2000 * MiB, "unknown avail keeps the configured budget");
    // Ample memory → configured fits under (avail - floor).
    check(entry_budget(2000 * MiB, 8 * GiB, 1536 * MiB, 64 * MiB) == 2000 * MiB,
          "ample memory keeps the configured budget");
    // Tight memory → clamp to (avail - floor).
    check(entry_budget(4000 * MiB, 3 * GiB, 1536 * MiB, 64 * MiB) == 3 * GiB - 1536 * MiB,
          "tight memory clamps to the sustainable budget");
    // Below the floor → never below min_cap (and never above configured).
    check(entry_budget(2000 * MiB, 1000 * MiB, 1536 * MiB, 64 * MiB) == 64 * MiB,
          "below the floor falls back to min_cap");
}

} // namespace

int main() {
    test_starts_at_initial();
    test_residency_triggers_a_cut();
    test_unmeasured_residency_is_not_pressure();
    test_fault_reflex_needs_a_baseline();
    test_small_fault_counts_are_noise();
    test_cooldown_collapses_a_burst();
    test_cuts_below_a_token_working_set_when_the_device_refuses_it();
    test_floor_is_respected_when_it_is_reachable();
    test_growth_is_additive_and_capped();
    test_growth_retreats_from_a_known_ceiling();
    test_probe_forgets_a_stale_ceiling();
    test_converges_on_a_synthetic_device();
    test_degenerate_caps();
    test_anon_war_does_not_poison_baseline_or_grow();
    test_anon_war_demotes_when_cuts_do_not_help();
    test_foreign_faults_do_not_cut();
    test_dense_decline_cuts_early_and_rewarms_once();
    test_infeasible_demand_demotes_immediately();
    test_rearm_promotes_when_room_returns();
    test_plateau_stops_growth_without_a_configured_cap();
    test_demotes_when_cut_below_token_demand();
    test_demotes_on_near_zero_hit_rate();
    test_demotes_on_sustained_fault_war();
    test_healthy_cache_is_not_demoted();
    test_entry_budget_helper();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("cache governor: all checks passed\n");
    return 0;
}
