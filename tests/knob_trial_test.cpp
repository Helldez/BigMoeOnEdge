// The knob trial's policy (core/src/engine/knob_trial.h), on synthetic decode steps: an arm's true
// seconds per token, a deterministic noise, a drift over the run, and a cache that fills before it
// settles. What is checked is the verdict and when the measuring starts, never a timing: the session
// does the timing, this decides on what it is given. Plus the record a verdict is kept as
// (bmoe/measured_knobs.h).

#include "../core/src/engine/knob_trial.h"
#include "bmoe/measured_knobs.h"

#include <cmath>
#include <cstdint>
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

// A reproducible noise in [-1, 1]: the same run every time, so a verdict is a property of the policy.
struct Noise {
    uint64_t x;
    double next() {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        return ((double) (x >> 11) / (double) (1ull << 53)) * 2.0 - 1.0;
    }
};

struct Sim {
    // Seconds per token of `arm` at step `t` of the run, before noise.
    std::function<double(int arm, int t)> spt;
    double noise = 0.0; // relative, uniform
    uint64_t seed = 1;
    // The cache's hit rate at step t (lookups per token fixed at 100); -1 for no cache.
    std::function<double(int t)> hit = [](int) { return 0.8; };
    int max_steps = 2000;
};

struct Outcome {
    bool done = false;
    int winner = -1;
    int steps = 0;         // steps fed until the verdict
    int settled_at = -1;   // the step the measuring started
    std::vector<int> arms; // the arm each step ran under
    std::vector<double> gains;
};

Outcome run(KnobTrial & k, const Sim & sim, int n_cand) {
    Outcome o;
    Noise n{sim.seed};
    for (int t = 0; t < sim.max_steps && !k.done(); ++t) {
        if (o.settled_at < 0 && !k.settling()) o.settled_at = t;
        const int arm = k.arm();
        o.arms.push_back(arm);
        const double s = sim.spt(arm, t) * (1.0 + sim.noise * n.next());
        const double h = sim.hit(t);
        if (h < 0.0)
            k.record(s, 1, 0, 0);
        else
            k.record(s, 1, 100, (uint64_t) std::lround(100.0 * h));
        o.steps = t + 1;
    }
    o.done = k.done();
    o.winner = k.winner();
    for (int c = 1; c <= n_cand; ++c)
        o.gains.push_back(k.gain(c));
    return o;
}

} // namespace

int main() {
    KnobTrialPolicy pol; // the defaults the session runs with, but for the window
    pol.window_tokens = 4;

    // ── noise alone: two equal arms under 10% noise per token never produce a winner ──
    {
        int wins = 0;
        for (uint64_t seed = 1; seed <= 50; ++seed) {
            KnobTrial k(1, false, pol);
            Sim sim;
            sim.spt = [](int, int) { return 0.050; };
            sim.noise = 0.10;
            sim.seed = seed;
            const Outcome o = run(k, sim, 1);
            wins += o.done && o.winner != 0;
        }
        check(wins == 0,
              "noise: equal arms under 10% noise, 50 seeds, no candidate wins (" + std::to_string(wins) + " did)");
    }

    // ── a real win: 25% faster wins every time under 10% noise ──
    {
        int wins = 0;
        for (uint64_t seed = 1; seed <= 50; ++seed) {
            KnobTrial k(1, false, pol);
            Sim sim;
            sim.spt = [](int arm, int) { return arm == 1 ? 0.040 : 0.050; };
            sim.noise = 0.10;
            sim.seed = seed;
            const Outcome o = run(k, sim, 1);
            wins += o.done && o.winner == 1;
        }
        check(wins == 50,
              "win: a candidate 25% faster under 10% noise wins on all 50 seeds (" + std::to_string(wins) + ")");
    }

    // ── a tie inside the margin: 5% faster is not a win ──
    {
        KnobTrial k(1, false, pol);
        Sim sim;
        sim.spt = [](int arm, int) { return arm == 1 ? 0.050 / 1.05 : 0.050; };
        const Outcome o = run(k, sim, 1);
        check(o.done && o.winner == 0, "tie: 5% faster, inside the 10% margin, holds the incumbent");
        check(std::fabs(o.gains[0] - 1.05) < 1e-9, "tie: the gain it reports is the 5% it measured");
    }
    {
        KnobTrial k(1, false, pol);
        Sim sim;
        sim.spt = [](int arm, int) { return arm == 1 ? 0.050 / 1.12 : 0.050; };
        const Outcome o = run(k, sim, 1);
        check(o.done && o.winner == 1, "margin: 12% faster, past the 10% margin, wins");
    }

    // ── drift: the machine slows to twice the time per token over the trial (a phone heating), or speeds up ──
    // A comparison of the incumbent first and the candidate after would call the candidate 30%+
    // slower or faster depending on the direction; bracketed windows see no difference.
    {
        bool ok = true;
        for (const double slope : {+0.03, -0.01}) {
            KnobTrial k(1, false, pol);
            Sim sim;
            sim.spt = [slope](int, int t) { return 0.050 * (1.0 + slope * t); };
            const Outcome o = run(k, sim, 1);
            ok &= o.done && o.winner == 0 && std::fabs(o.gains[0] - 1.0) < 0.01;
        }
        check(ok, "drift: a linear slowdown or speedup over the run, equal arms: no winner, gain within 1%");
    }
    {
        // Drift and a real 20% win together: the win survives a slowdown that is larger than it.
        KnobTrial k(1, false, pol);
        Sim sim;
        sim.spt = [](int arm, int t) { return (arm == 1 ? 0.050 / 1.2 : 0.050) * (1.0 + 0.03 * t); };
        const Outcome o = run(k, sim, 1);
        check(o.done && o.winner == 1 && std::fabs(o.gains[0] - 1.2) < 0.05,
              "drift: a 20% win under a slowdown of 2x over the run is still measured as ~20%");
    }

    // ── one stalled window does not decide it: the median of the rounds does ──
    {
        KnobTrial k(1, false, pol);
        Sim sim;
        // The candidate is 25% faster, but one of its windows met a stall elsewhere in the machine.
        sim.spt = [](int arm, int t) {
            const double base = arm == 1 ? 0.040 : 0.050;
            return (t >= 6 && t < 10) ? base * 8.0 : base;
        };
        const Outcome o = run(k, sim, 1);
        check(o.done && o.winner == 1, "outlier: one stalled window does not overturn three rounds' median");
    }

    // ── two candidates: the faster one past the margin wins, and the schedule brackets each ──
    {
        KnobTrial k(2, false, pol);
        Sim sim;
        sim.spt = [](int arm, int) { return arm == 0 ? 0.050 : arm == 1 ? 0.045 : 0.038; };
        const Outcome o = run(k, sim, 2);
        check(o.done && o.winner == 2, "two candidates: the one 32% faster wins over the one 11% faster");
        const std::vector<int> & sch = k.schedule();
        bool bracketed = sch.front() == 0 && sch.back() == 0 && sch.size() == 1 + 2 * 2 * (size_t) pol.rounds;
        for (size_t i = 1; i + 1 < sch.size(); i += 2)
            bracketed &= sch[i] != 0 && sch[i - 1] == 0 && sch[i + 1] == 0;
        check(bracketed, "two candidates: every candidate window sits between two incumbent windows");
    }

    // ── settling: nothing is measured while the cache fills ──
    {
        KnobTrial k(1, true, pol);
        Sim sim;
        sim.spt = [](int arm, int) { return arm == 1 ? 0.040 : 0.050; };
        // The hit rate climbs 10 points a window for the first 24 steps, then holds.
        sim.hit = [](int t) { return t < 24 ? 0.2 + 0.025 * t : 0.8; };
        const Outcome o = run(k, sim, 1);
        bool incumbent_while_settling = true;
        for (int t = 0; t < o.settled_at && t < (int) o.arms.size(); ++t)
            incumbent_while_settling &= o.arms[t] == 0;
        check(o.settled_at >= 24,
              "settle: measuring starts only once two windows hit alike (step " + std::to_string(o.settled_at) + ")");
        check(incumbent_while_settling, "settle: every step before it runs the incumbent");
        check(o.done && o.winner == 1, "settle: the trial then measures and decides");
    }
    {
        KnobTrial k(1, true, pol);
        Sim sim;
        sim.spt = [](int, int) { return 0.050; };
        sim.hit = [](int t) { return (t / 4) % 2 ? 0.2 : 0.9; }; // never settles
        const Outcome o = run(k, sim, 1);
        check(o.settled_at == pol.settle_windows_max * pol.window_tokens + pol.warm_tokens,
              "settle: a cache that never settles is measured after the policy's window count");
    }
    {
        KnobTrial k(1, true, pol);
        Sim sim;
        sim.spt = [](int, int) { return 0.050; };
        sim.hit = [](int) { return -1.0; }; // no cache
        const Outcome o = run(k, sim, 1);
        check(o.settled_at == pol.window_tokens + pol.warm_tokens,
              "settle: with no cache there is nothing to wait for");
    }

    // ── warm-up and interruption ──
    {
        KnobTrial k(1, false, pol);
        // The first window opens after the warm-up step, which is not timed.
        k.record(100.0, 1, 0, 0); // a cold step: would dominate any window it entered
        for (int i = 0; i < pol.window_tokens; ++i)
            k.record(0.05, 1, 0, 0);
        const bool first_ok =
            k.window_seconds_per_token().size() == 1 && std::fabs(k.window_seconds_per_token()[0] - 0.05) < 1e-12;
        check(first_ok, "warm-up: the step after a switch is not timed");
        // A request ends inside a candidate window: what it had is dropped.
        k.record(0.04, 1, 0, 0); // warm-up of the switch to the candidate
        k.record(9.0, 1, 0, 0);
        k.interrupt();
        k.record(9.0, 1, 0, 0); // the warm-up again, after the next prompt
        for (int i = 0; i < pol.window_tokens; ++i)
            k.record(0.04, 1, 0, 0);
        const auto & w = k.window_seconds_per_token();
        check(w.size() == 2 && std::fabs(w[1] - 0.04) < 1e-12 && k.arm() == 0,
              "interrupt: a window cut by the end of a request is measured afresh, not merged");
    }
    {
        // A speculative step yields several tokens at once: it is timed whole.
        KnobTrial k(1, false, pol);
        k.record(0.05, 1, 0, 0); // warm-up
        k.record(0.20, 4, 0, 0);
        check(k.window_seconds_per_token().size() == 1 && std::fabs(k.window_seconds_per_token()[0] - 0.05) < 1e-12,
              "speculation: a step of four tokens counts four tokens of its wall");
    }
    {
        KnobTrial none(0, true, pol);
        check(none.done() && none.winner() == 0 && none.arm() == 0, "no candidates: done at once, the incumbent");
    }

    // ── the record a verdict is kept as ──
    {
        MeasuredKnobs a;
        a.hardware = "00112233aabbccdd";
        a.engine = "0.35.1";
        a.threads = 6;
        a.threads_tried = {8, 6, 10};
        a.threads_gain = 1.1534;
        a.overlap = 1;
        a.overlap_incumbent = 0;
        a.overlap_gain = 1.12;
        a.rounds = 3;
        MeasuredKnobs b = a;
        b.hardware = "ffeeddccbbaa0011";
        b.overlap = -1;
        b.overlap_incumbent = -1;
        std::vector<MeasuredKnobs> back;
        std::string err;
        const bool parsed = parse_measured_knobs(format_measured_knobs({a, b}), back, err);
        check(parsed && back.size() == 2 && back[0].threads == 6 && back[0].threads_tried == a.threads_tried &&
                  std::fabs(back[0].threads_gain - 1.1534) < 1e-9 && back[0].overlap == 1 &&
                  back[0].overlap_incumbent == 0 && back[1].overlap == -1 && back[1].hardware == b.hardware,
              "record: two machines' verdicts read back as written" + (parsed ? "" : " (" + err + ")"));
        check(find_measured_knobs(back, a.hardware, "0.35.1") && !find_measured_knobs(back, a.hardware, "0.36.0"),
              "record: a verdict is this machine's AND this engine's");
        MeasuredKnobs a2 = a;
        a2.threads = 8;
        keep_measured_knobs(back, a2);
        check(back.size() == 2 && find_measured_knobs(back, a.hardware, "0.35.1")->threads == 8,
              "record: keeping a machine's verdict replaces the one it had");
        std::vector<MeasuredKnobs> bad;
        check(!parse_measured_knobs("bmoe-measured-knobs 1\nthreads=4\n", bad, err),
              "record: a line naming no machine is refused");
        check(!parse_measured_knobs("something else\n", bad, err), "record: another format is refused");
        check(parse_measured_knobs("bmoe-measured-knobs 1\nhardware=a engine=b threads=4 later_key=1\n", bad, err) &&
                  bad.size() == 1 && bad[0].threads == 4,
              "record: a key a later engine added is read past");
    }
    {
        HardwareProfile hw;
        hw.n_cores = 10;
        hw.core_classes = {4, 6};
        hw.memory_total = 16ull << 30;
        hw.residency_budget = 9ull << 30;
        ComputeDevice gpu;
        gpu.name = "GPU0";
        gpu.memory_total = 8ull << 30;
        gpu.memory_free = 6ull << 30;
        hw.devices.push_back(gpu);
        const std::string fp = hardware_fingerprint(hw);
        HardwareProfile later = hw;
        later.residency_budget = 4ull << 30; // the budget moves between loads
        later.devices[0].memory_free = 1ull << 30;
        later.host_bandwidth_gibs = 77.0; // and so does every measured rate
        later.best_threads = 6;
        check(fp.size() == 16 && hardware_fingerprint(later) == fp,
              "fingerprint: free memory and measured rates do not change the machine");
        HardwareProfile other = hw;
        other.devices[0].memory_total = 16ull << 30;
        HardwareProfile fewer = hw;
        fewer.core_classes = {4, 4};
        fewer.n_cores = 8;
        check(hardware_fingerprint(other) != fp && hardware_fingerprint(fewer) != fp,
              "fingerprint: another device or another processor is another machine");
    }

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall knob-trial checks passed\n");
    return 0;
}
