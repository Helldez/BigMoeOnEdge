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
    int apart = 0; // arms given up on one window apart from the incumbent
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
        o.apart += k.last_abandoned() && k.last_abandoned_apart();
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
        // The candidate is 25% faster, but one of its windows met a stall elsewhere in the machine,
        // which slowed three of its four tokens. A slowdown of every token of a window past the
        // incumbent's whole range is judged as the arm's own (the `apart` checks below).
        sim.spt = [](int arm, int t) {
            const double base = arm == 1 ? 0.040 : 0.050;
            return (t >= 7 && t < 11) ? base * 8.0 : base;
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
        std::vector<MeasuredKnobs> bad;
        MeasuredKnobs a2 = a;
        a2.threads = 8;
        keep_measured_knobs(back, a2);
        check(back.size() == 2 && find_measured_knobs(back, a.hardware, "0.35.1")->threads == 8,
              "record: keeping a machine's verdict replaces the one it had");
        // The device tier is measured across loads: each load brings one side, and keeping it must not
        // lose the other side or the knobs an earlier load measured.
        MeasuredKnobs on;
        on.hardware = a.hardware;
        on.engine = a.engine;
        on.device_cache_tps = 1.6;
        keep_measured_knobs(back, on);
        MeasuredKnobs off = on;
        off.device_cache_tps = 0.0;
        off.no_device_cache_tps = 3.94;
        keep_measured_knobs(back, off);
        const MeasuredKnobs * kept = find_measured_knobs(back, a.hardware, "0.35.1");
        check(kept && kept->threads == 8 && kept->overlap == 1 && std::fabs(kept->device_cache_tps - 1.6) < 1e-9 &&
                  std::fabs(kept->no_device_cache_tps - 3.94) < 1e-9 && kept->verdicts() == 4,
              "record: each load's side of the device tier is merged, nothing an earlier load measured is lost");
        std::vector<MeasuredKnobs> again;
        check(parse_measured_knobs(format_measured_knobs(back), again, err) && again.size() == 2 &&
                  std::fabs(find_measured_knobs(again, a.hardware, "0.35.1")->no_device_cache_tps - 3.94) < 1e-3,
              "record: the device tier's rates read back as written");
        check(!parse_measured_knobs("bmoe-measured-knobs 1\nhardware=a engine=b device_cache_tps=-1\n", bad, err),
              "record: a negative rate is refused");
        // The graph width: each side's rates and the usage they are weighed by, merged across loads; a
        // different narrow width starts the comparison afresh.
        MeasuredKnobs w1;
        w1.hardware = a.hardware;
        w1.engine = a.engine;
        w1.width_narrow = 512;
        w1.wide_decode_tps = 14.2;
        w1.wide_prefill_tps = 610.0;
        w1.usage_requests = 3;
        w1.usage_gen_tokens = 480.0;
        w1.usage_long_prompt_tokens = 9500.0;
        keep_measured_knobs(back, w1);
        MeasuredKnobs w2 = w1;
        w2.wide_decode_tps = w2.wide_prefill_tps = 0.0;
        w2.narrow_decode_tps = 19.4;
        w2.narrow_prefill_tps = 104.0;
        keep_measured_knobs(back, w2);
        const MeasuredKnobs * kw = find_measured_knobs(back, a.hardware, "0.35.1");
        check(kw && kw->width_narrow == 512 && std::fabs(kw->wide_decode_tps - 14.2) < 1e-9 &&
                  std::fabs(kw->narrow_prefill_tps - 104.0) < 1e-9 && kw->usage_requests == 3 &&
                  std::fabs(kw->device_cache_tps - 1.6) < 1e-9,
              "record: the width's two sides are merged, with what else was measured");
        std::vector<MeasuredKnobs> wback;
        check(parse_measured_knobs(format_measured_knobs(back), wback, err) &&
                  find_measured_knobs(wback, a.hardware, "0.35.1")->width_narrow == 512 &&
                  std::fabs(find_measured_knobs(wback, a.hardware, "0.35.1")->usage_long_prompt_tokens - 9500.0) < 1e-3,
              "record: the width's rates and usage read back as written");
        MeasuredKnobs w3 = w1;
        w3.width_narrow = 1024;
        w3.wide_prefill_tps = 0.0;
        w3.wide_decode_tps = 15.0;
        keep_measured_knobs(back, w3);
        kw = find_measured_knobs(back, a.hardware, "0.35.1");
        check(kw && kw->width_narrow == 1024 && std::fabs(kw->wide_decode_tps - 15.0) < 1e-9 &&
                  kw->narrow_decode_tps == 0.0 && kw->narrow_prefill_tps == 0.0,
              "record: another narrow width drops what was measured against the old one");
        check(!parse_measured_knobs("bmoe-measured-knobs 1\nthreads=4\n", bad, err),
              "record: a line naming no machine is refused");
        check(!parse_measured_knobs("something else\n", bad, err), "record: another format is refused");
        check(parse_measured_knobs("bmoe-measured-knobs 1\nhardware=a engine=b threads=4 later_key=1\n", bad, err) &&
                  bad.size() == 1 && bad[0].threads == 4,
              "record: a key a later engine added is read past");

        // The decode's own cost: read back as written, merged whole, counted so a front-end keeps it,
        // and absent from a record an earlier engine wrote.
        MeasuredKnobs dc;
        dc.hardware = a.hardware;
        dc.engine = a.engine;
        dc.decode_compute_s = 0.064125;
        dc.decode_threads = 4;
        dc.decode_hit = 0.8125;
        dc.decode_cache_mib = 1536.0;
        dc.decode_gpu_layers = 0;
        dc.decode_device_cache_mb = 0;
        check(dc.verdicts() == 2, "record: a measured decode cost counts, so a front-end keeps the grown record");
        std::vector<MeasuredKnobs> dback;
        check(parse_measured_knobs(format_measured_knobs({dc}), dback, err) && dback.size() == 1 &&
                  std::fabs(dback[0].decode_compute_s - 0.064125) < 1e-9 && dback[0].decode_threads == 4 &&
                  std::fabs(dback[0].decode_hit - 0.8125) < 1e-9 &&
                  std::fabs(dback[0].decode_cache_mib - 1536.0) < 1e-9,
              "record: the decode's cost reads back as written");
        keep_measured_knobs(back, dc);
        const MeasuredKnobs * kd = find_measured_knobs(back, a.hardware, "0.35.1");
        check(kd && kd->decode_threads == 4 && std::fabs(kd->decode_hit - 0.8125) < 1e-9 && kd->width_narrow == 1024,
              "record: the decode's cost is merged beside what else was measured");
        MeasuredKnobs dc2 = dc;
        dc2.decode_compute_s = 0.05;
        dc2.decode_threads = 6;
        dc2.decode_hit = -1.0;
        dc2.decode_cache_mib = 0.0;
        keep_measured_knobs(back, dc2);
        kd = find_measured_knobs(back, a.hardware, "0.35.1");
        check(kd && kd->decode_threads == 6 && kd->decode_hit == -1.0 && std::fabs(kd->decode_compute_s - 0.05) < 1e-12,
              "record: a later load's decode cost replaces the whole measurement, never half of it");
        std::vector<MeasuredKnobs> old;
        check(parse_measured_knobs("bmoe-measured-knobs 1\nhardware=a engine=b threads=4 overlap=1 rounds=3\n", old,
                                   err) &&
                  old.size() == 1 && old[0].decode_compute_s == 0.0 && old[0].decode_hit == -1.0 &&
                  old[0].decode_threads == 0 && old[0].verdicts() == 2,
              "record: one written before the decode's cost was recorded reads as not measured");
        check(!parse_measured_knobs("bmoe-measured-knobs 1\nhardware=a engine=b decode_hit=1.5\n", bad, err),
              "record: a hit rate past 1 is refused");

        // Sides a calibration could not open (bmoe/calibrate.h): written only when there is one, read
        // back, counted, cleared by a rate for the same side, and read past by an engine that does not
        // know a side's name.
        MeasuredKnobs uv;
        uv.hardware = a.hardware;
        uv.engine = "0.42.0";
        check(format_measured_knobs({uv}).find("unviable") == std::string::npos,
              "record: no side failed, no unviable key");
        uv.no_device_cache_unviable = true;
        uv.width_narrow = 2048;
        uv.narrow_unviable = true;
        std::vector<MeasuredKnobs> uback;
        check(parse_measured_knobs(format_measured_knobs({uv}), uback, err) && uback.size() == 1 &&
                  uback[0].no_device_cache_unviable && uback[0].narrow_unviable && !uback[0].device_cache_unviable &&
                  !uback[0].wide_unviable && uback[0].verdicts() == 2,
              "record: the sides that could not open read back and count as verdicts");
        check(
            parse_measured_knobs("bmoe-measured-knobs 1\nhardware=a engine=b unviable=wide,later_side\n", uback, err) &&
                uback.size() == 1 && uback[0].wide_unviable,
            "record: a side name from a later engine is read past");
        check(!parse_measured_knobs("bmoe-measured-knobs 1\nhardware=a engine=b unviable=\n", uback, err),
              "record: an empty side list is refused");
        std::vector<MeasuredKnobs> ukept = {uv};
        MeasuredKnobs timed_later;
        timed_later.hardware = uv.hardware;
        timed_later.engine = uv.engine;
        timed_later.no_device_cache_tps = 8.0;
        keep_measured_knobs(ukept, timed_later);
        check(!ukept[0].no_device_cache_unviable && ukept[0].no_device_cache_tps == 8.0 && ukept[0].narrow_unviable,
              "record: a side timed later is viable after all; the other failure stays");
        MeasuredKnobs other_narrow;
        other_narrow.hardware = uv.hardware;
        other_narrow.engine = uv.engine;
        other_narrow.width_narrow = 4096;
        keep_measured_knobs(ukept, other_narrow);
        check(!ukept[0].narrow_unviable && ukept[0].width_narrow == 4096,
              "record: a failure at another narrow width answers nothing at the new one");

        // The request mix: recorded on every load, read back as written, counted so a front-end keeps
        // it as it grows, replaced whole by a later load that served requests, and absent from a record
        // an earlier engine wrote. The host's prefill rate survives a load whose long prompts all ran
        // on a device.
        MeasuredKnobs mx;
        mx.hardware = a.hardware;
        mx.engine = a.engine;
        mx.mix_requests = 4;
        mx.mix_gen_tokens = 120.5;
        mx.mix_wide_prompt_tokens = 1054.0;
        mx.host_prefill_tps = 8.64;
        check(mx.verdicts() == 5, "record: each request of the mix counts, so a front-end keeps the grown record");
        std::vector<MeasuredKnobs> mback;
        check(parse_measured_knobs(format_measured_knobs({mx}), mback, err) && mback.size() == 1 &&
                  mback[0].mix_requests == 4 && std::fabs(mback[0].mix_gen_tokens - 120.5) < 1e-9 &&
                  std::fabs(mback[0].mix_wide_prompt_tokens - 1054.0) < 1e-9 &&
                  std::fabs(mback[0].host_prefill_tps - 8.64) < 1e-9,
              "record: the request mix reads back as written");
        keep_measured_knobs(back, mx);
        MeasuredKnobs mx2 = mx;
        mx2.mix_requests = 1;
        mx2.mix_gen_tokens = 2000.0;
        mx2.mix_wide_prompt_tokens = 0.0;
        mx2.host_prefill_tps = 0.0; // every long prompt of this load ran on the device, or there was none
        keep_measured_knobs(back, mx2);
        const MeasuredKnobs * km = find_measured_knobs(back, a.hardware, "0.35.1");
        check(km && km->mix_requests == 1 && km->mix_gen_tokens == 2000.0 && km->mix_wide_prompt_tokens == 0.0 &&
                  std::fabs(km->host_prefill_tps - 8.64) < 1e-9 && km->decode_threads == 6,
              "record: a later load's mix replaces the mix whole, and keeps the host's prefill rate it did not "
              "measure");
        check(old[0].mix_requests == 0 && old[0].host_prefill_tps == 0.0,
              "record: one written before the mix was recorded reads as not recorded");
        check(!parse_measured_knobs("bmoe-measured-knobs 1\nhardware=a engine=b mix_requests=-1\n", bad, err),
              "record: a negative request count is refused");
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

    // ── settled: a candidate that can no longer win stops being measured ──
    // Three rounds of eight tokens at ten times the pace tripled a phone's first answer; with three
    // rounds, two losing ones settle the median, and the third would only repeat them. The brake
    // must not read those windows either (the session's concern). An ordinary loss, 20% under 15%
    // noise per token, whose windows overlap the incumbent's: the median settles it, not one window.
    {
        KnobTrial k(1, false, pol);
        Sim sim;
        sim.spt = [](int arm, int) { return arm == 1 ? 0.060 : 0.050; };
        sim.noise = 0.15;
        const Outcome o = run(k, sim, 1);
        int cand_steps = 0;
        for (int a : o.arms)
            cand_steps += a == 1;
        check(o.done && o.winner == 0, "settled: 20% slower holds the incumbent");
        check(k.abandoned(1) && k.rounds(1) == 2 && o.apart == 0,
              "settled: the candidate stopped on the median, with two rounds on record (" +
                  std::to_string(k.rounds(1)) + ")");
        check(cand_steps == 2 * (pol.window_tokens + pol.warm_tokens),
              "settled: the candidate ran two windows and their warm-ups only (" + std::to_string(cand_steps) +
                  " steps)");
        // Incumbent, candidate, incumbent, candidate, incumbent: five windows, each after a warm-up.
        check(o.steps <= 5 * (pol.window_tokens + pol.warm_tokens),
              "settled: the trial ended after the bracketing window (" + std::to_string(o.steps) + " steps)");
    }
    // ── apart: a window slower in every token than the incumbent's whole range ends the candidate ──
    // On a phone, eight decode threads against four ran each token about twenty times slower, and the
    // median's second losing window was another 18 s of stutter. One window decides that: its fastest
    // token is past the incumbent's slowest by more than the incumbent's spread and the margin.
    {
        KnobTrial k(1, false, pol);
        Sim sim;
        sim.spt = [](int arm, int) { return arm == 1 ? 1.000 : 0.050; };
        sim.noise = 0.10;
        const Outcome o = run(k, sim, 1);
        int cand_steps = 0;
        for (int a : o.arms)
            cand_steps += a == 1;
        check(o.done && o.winner == 0 && k.abandoned(1) && k.rounds(1) == 1 && o.apart == 1,
              "apart: twenty times slower stops after one round (" + std::to_string(k.rounds(1)) + ")");
        // Cut inside its window: once more than half of its tokens are past the bar, and with them
        // the seconds the whole window may spend.
        check(cand_steps == pol.warm_tokens + pol.window_tokens / 2 + 1,
              "apart: the candidate's window is cut after more than half of it (" + std::to_string(cand_steps) +
                  " steps)");
        check(o.steps <= 3 * (pol.window_tokens + pol.warm_tokens),
              "apart: the trial ended after the bracketing window (" + std::to_string(o.steps) + " steps)");
        check(k.gain(1) > 0.0 && k.gain(1) < 0.1, "apart: the round it ran stays on record as the loss it measured");
    }
    // ── apart beside another candidate: the slow one leaves the schedule, the other is measured through ──
    // The phone's trial: {2, 8} against 4, eight the collapse. Its later windows are skipped, the
    // other candidate keeps its rounds.
    {
        KnobTrial k(2, false, pol);
        Sim sim;
        sim.spt = [](int arm, int) { return arm == 2 ? 1.000 : arm == 1 ? 0.040 : 0.050; };
        sim.noise = 0.05;
        const Outcome o = run(k, sim, 2);
        int slow_steps = 0;
        for (int a : o.arms)
            slow_steps += a == 2;
        check(o.done && o.winner == 1 && k.abandoned(2) && !k.abandoned(1) && k.rounds(2) == 1 &&
                  k.rounds(1) == pol.rounds,
              "apart: the collapsing candidate stops after one round, the 25% faster one keeps its rounds and wins");
        check(slow_steps == pol.warm_tokens + pol.window_tokens / 2 + 1,
              "apart: the schedule skips the collapsing candidate's later windows (" + std::to_string(slow_steps) +
                  " steps)");
        bool bracketed = true;
        const std::vector<int> & s = k.schedule();
        for (size_t j = 0; j < s.size(); ++j)
            if (s[j] != 0 && (j == 0 || j + 1 >= s.size() || s[j - 1] != 0 || s[j + 1] != 0)) bracketed = false;
        check(bracketed && s.front() == 0 && s.back() == 0,
              "apart: the schedule left keeps every candidate window between two incumbent windows");
    }
    // ── settled beside a real win: the slow candidate stops, the fast one is measured through and wins ──
    {
        KnobTrial k(2, false, pol);
        Sim sim;
        sim.spt = [](int arm, int) { return arm == 1 ? 0.500 : arm == 2 ? 0.040 : 0.050; };
        sim.noise = 0.05;
        const Outcome o = run(k, sim, 2);
        check(o.done && o.winner == 2 && k.abandoned(1) && !k.abandoned(2) && k.rounds(2) == pol.rounds,
              "settled: the slow candidate stops, the 25% faster one keeps its rounds and wins");
    }
    // ── a few percent slower in one window settles nothing: inside the margin, not apart ──
    {
        KnobTrial k(1, false, pol);
        Sim sim;
        sim.spt = [](int arm, int) { return arm == 1 ? 0.0525 : 0.050; };
        const Outcome o = run(k, sim, 1);
        check(o.done && o.winner == 0 && o.apart == 0 && k.rounds(1) == 2,
              "apart: 5% slower in every token is not ended by one window; the median settles it after two");
    }
    // ── one losing round settles nothing: a 25% faster candidate whose first window stalled ──
    // A stall slows some tokens of a window: the window's average is past the incumbent's by far, its
    // fastest token is not, and the median decides as before.
    {
        KnobTrial k(1, false, pol);
        Sim sim;
        sim.spt = [](int arm, int t) {
            if (arm != 1) return 0.050;
            return (t >= 6 && t < 8) ? 0.400 : 0.040; // half of its first window
        };
        const Outcome o = run(k, sim, 1);
        check(o.done && o.winner == 1 && !k.abandoned(1) && k.rounds(1) == pol.rounds,
              "settled: one stalled round is measured through, and the candidate still wins");
    }
    // ── the incumbent's own spread raises the bar: a loss inside it is not ended by one window ──
    // Its tokens alternate 50 and 100 ms (a range of two); a candidate at 150 ms a token, twice the
    // incumbent's mean, is still inside what that range allows, and the median decides it. The same
    // candidate against a steady incumbent at the same mean is apart, and ends after one window.
    {
        KnobTrial k(1, false, pol);
        Sim sim;
        sim.spt = [](int arm, int t) { return arm == 1 ? 0.150 : (t % 2 ? 0.100 : 0.050); };
        const Outcome o = run(k, sim, 1);
        check(o.done && o.winner == 0 && o.apart == 0 && k.rounds(1) == 2,
              "spread: a loss within the incumbent's own range is settled by the median, not one window");
        KnobTrial steady(1, false, pol);
        Sim flat;
        flat.spt = [](int arm, int) { return arm == 1 ? 0.150 : 0.075; };
        const Outcome f = run(steady, flat, 1);
        check(f.done && f.winner == 0 && f.apart == 1 && steady.rounds(1) == 1,
              "spread: the same loss against a steady incumbent is apart after one window");
    }
    // ── the phone's collapse at the session's own window: cut after more than half of it ──
    // Four threads near 0.11 s a token, eight at 2.4-3.0 s: the window #539 left ran nine steps of
    // stutter, where five past the bar (and the seconds the whole window may spend) decide it.
    {
        KnobTrialPolicy session_pol; // the session's window
        KnobTrial k(1, false, session_pol);
        Sim sim;
        sim.spt = [](int arm, int) { return arm == 1 ? 2.6 : 0.11; };
        sim.noise = 0.15;
        const Outcome o = run(k, sim, 1);
        int cand_steps = 0;
        for (int a : o.arms)
            cand_steps += a == 1;
        check(o.done && o.winner == 0 && o.apart == 1 && k.rounds(1) == 1 &&
                  cand_steps == session_pol.warm_tokens + session_pol.window_tokens / 2 + 1,
              "cut: a collapse at the session's window costs its warm-up and five tokens, not nine (" +
                  std::to_string(cand_steps) + " steps)");
    }
    // ── a stall over half of a window cuts nothing: the faster candidate is measured through and wins ──
    {
        KnobTrialPolicy session_pol;
        KnobTrial k(1, false, session_pol);
        Sim sim;
        const int first_cand = session_pol.warm_tokens + session_pol.window_tokens + session_pol.warm_tokens;
        sim.spt = [first_cand, &session_pol](int arm, int t) {
            if (arm != 1) return 0.050;
            return (t >= first_cand && t < first_cand + session_pol.window_tokens / 2) ? 0.800 : 0.040;
        };
        const Outcome o = run(k, sim, 1);
        check(o.done && o.winner == 1 && !k.abandoned(1) && o.apart == 0,
              "cut: a stall over half of the candidate's window is not a collapse; it still wins");
    }
    // ── a tie inside the margin is settled too: measured for two rounds, not three ──
    {
        KnobTrial k(1, false, pol);
        Sim sim;
        sim.spt = [](int arm, int) { return arm == 1 ? 0.050 / 1.05 : 0.050; };
        const Outcome o = run(k, sim, 1);
        check(o.done && o.winner == 0 && k.abandoned(1) && k.rounds(1) == 2,
              "settled: 5% faster cannot reach the margin after two rounds and stops there");
    }

    // ── the rate probe: one side of the device tier, timed ──
    {
        KnobTrialPolicy rp;
        rp.window_tokens = 4;
        // A cache in view settles as the trial does, then times 2 * rounds + 1 windows and gives their median.
        RateProbe r(/*settle*/ true, /*hits_visible*/ true, rp);
        // One step a window (the first is the warm-up), one stalled window in five.
        for (int w = 0; w < 1000 && !r.done(); ++w) {
            const double spt = w % 5 == 3 ? 0.5 : 0.1;
            r.record(spt * rp.window_tokens, rp.window_tokens, 100, 90);
        }
        check(r.done() && r.windows() == r.windows_wanted() && r.windows_wanted() == 2 * rp.rounds + 1 &&
                  std::fabs(r.tokens_per_second() - 10.0) < 1e-6,
              "rate probe: the median of 2 * rounds + 1 windows, a stalled window does not move it");
        check(std::fabs(r.hit_rate() - 0.9) < 1e-12, "rate probe: the hit rate of the timed windows");
        check(RateProbe(false, true, rp).hit_rate() == -1.0, "rate probe: no lookups, no hit rate");
        // A device cache out of view: the longest settle the trial makes, whatever the counters say.
        RateProbe blind(true, /*hits_visible*/ false, rp);
        int steps = 0;
        while (blind.settling() && steps < 10000) {
            blind.record(0.1, 1, 0, 0);
            ++steps;
        }
        check(steps == rp.settle_windows_max * rp.window_tokens + rp.warm_tokens,
              "rate probe: a cache out of view waits settle_windows_max windows");
        RateProbe now(/*settle*/ false, true, rp);
        check(!now.settling() && now.tokens_per_second() == 0.0, "rate probe: no settle, no rate before it is done");
        now.record(0.1, 2, 0, 0);
        now.interrupt();
        now.record(0.1, 1, 0, 0); // the warm-up after an interruption is not timed
        check(now.windows() == 0, "rate probe: an interruption drops the window in progress");
    }

    // ── the helpers a load-time calibration shares with the trial ──
    {
        // The bar is the reference's slowest step times its range or the margin, whichever is wider.
        check(std::fabs(apart_bar(0.10, 0.11, 0.05) - 0.11 * 1.1) < 1e-12, "apart bar: a noisy reference, its range");
        check(std::fabs(apart_bar(0.10, 0.10, 0.05) - 0.10 * 1.05) < 1e-12, "apart bar: a steady one, the margin");
        check(apart_bar(0.0, 0.1, 0.05) == 0.0, "apart bar: no reference, no bar");
        check(within_margin(0.100, 0.104, 0.05) && within_margin(0.104, 0.100, 0.05) &&
                  !within_margin(0.100, 0.110, 0.05) && !within_margin(0.0, 0.1, 0.05),
              "within margin: either way round, and never against nothing");
    }
    // ── the rate probe settled on the rate where the hits are out of view ──
    {
        KnobTrialPolicy rp;
        rp.window_tokens = 4;
        // A device cache filling: the first windows slow, then steady. On the rate, the probe starts
        // timing at the first two windows that agree, far before the longest wait.
        const auto windows_to_settle = [&rp](RateProbe & r, const std::function<double(int)> & spt) {
            for (int i = 0; i < rp.warm_tokens; ++i)
                r.record(1.0, 1, 0, 0); // the warm-up after the prefill, not timed
            int w = 0;
            for (; w < 100 && r.settling(); ++w)
                for (int i = 0; i < rp.window_tokens; ++i)
                    r.record(spt(w), 1, 0, 0);
            return w;
        };
        RateProbe on_rate(/*settle*/ true, /*hits_visible*/ false, rp);
        on_rate.settle_on_rate();
        const int settled = windows_to_settle(on_rate, [](int w) { return w == 0 ? 0.5 : w == 1 ? 0.3 : 0.1; });
        check(settled == 4 && settled < rp.settle_windows_max,
              "rate settle: two windows of one rate end the wait, " + std::to_string(settled) + " windows");
        RateProbe never(true, false, rp);
        never.settle_on_rate();
        check(windows_to_settle(never, [](int w) { return w % 2 ? 0.3 : 0.1; }) == rp.settle_windows_max,
              "rate settle: a rate that never steadies still ends at the longest wait");
        RateProbe blind(true, false, rp);
        check(windows_to_settle(blind, [](int) { return 0.1; }) == rp.settle_windows_max,
              "rate settle: off, a cache out of view still waits the longest");
        check(RateProbe(true, true, rp).tokens_max() ==
                  rp.warm_tokens + (rp.settle_windows_max + 2 * rp.rounds + 1) * rp.window_tokens,
              "rate probe: the most tokens it can take");
    }
    // ── the rate probe set against the other side: cut once past its range, never during the settle ──
    {
        KnobTrialPolicy rp;
        rp.window_tokens = 4;
        RateProbe slow(/*settle*/ false, true, rp);
        slow.set_reference(0.050, 0.055);
        int steps = 0;
        while (!slow.done() && steps < 1000) {
            slow.record(0.080, 1, 0, 0);
            ++steps;
        }
        check(slow.done() && slow.cut() && slow.windows() == 1 && std::fabs(slow.tokens_per_second() - 12.5) < 1e-9,
              "cut: one window past the reference's range ends it, its rate kept");
        RateProbe close(false, true, rp);
        close.set_reference(0.050, 0.055);
        for (int i = 0; i < 1000 && !close.done(); ++i)
            close.record(0.056, 1, 0, 0);
        check(close.done() && !close.cut() && close.windows() == close.windows_wanted(),
              "cut: a side inside the bar runs every window");
        check(std::fabs(close.step_lo() - 0.056) < 1e-12 && std::fabs(close.step_hi() - 0.056) < 1e-12,
              "rate probe: the timed steps' range, for the other side's cut");
        RateProbe cold(true, true, rp);
        cold.set_reference(0.050, 0.055);
        for (int i = 0; i < 40 && cold.settling(); ++i)
            cold.record(0.5, 1, 100, (uint64_t) (i * 2)); // a cache filling: slow, and never past the bar's say
        check(!cold.cut(), "cut: not while the cache is settling");
        RateProbe partial(false, true, rp);
        for (int i = 0; i < rp.warm_tokens + 2 * rp.window_tokens; ++i)
            partial.record(0.1, 1, 0, 0);
        check(!partial.done() && partial.tokens_per_second() == 0.0 && std::fabs(partial.rate_so_far() - 10.0) < 1e-9,
              "rate so far: the windows timed before the answer ended");
    }

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall knob-trial checks passed\n");
    return 0;
}
