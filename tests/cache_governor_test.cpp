// The cache governor is pure policy, so its device is a function: "faults iff the budget exceeds
// what I concede". That makes the properties that actually matter testable without a phone —
// that it converges just under an unobservable concession and stops, that it keeps cutting for as
// long as the device keeps taking, that it never cuts below the layer it must stage — none of which
// a byte-identity gate can see, because the governor changes what is resident and never what is
// computed.

#include "bmoe/cache_governor.h"

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

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("cache governor: all checks passed\n");
    return 0;
}
