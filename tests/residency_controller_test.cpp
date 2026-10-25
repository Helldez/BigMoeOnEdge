// The residency controller's policy, replayed on per-token traces measured on real machines.
//
// The traces are the `compute_ms` (and, where kept, `swap_mib`) columns of runs that were kept: a
// 12 GB phone whose system took the engine's memory back at the default budget and ended the run, the
// same phone at a small budget that ran clean, a 16 GB desktop at the same small budget, and a 16 GB
// laptop whose locked cache and dense weights left its compute buffers to be compressed. What is
// checked is when the policy calls a stall, when a stall moves the budget, and how a run climbs back.

#include "../core/src/engine/residency_controller.h"

#include <cstdio>
#include <vector>

using bmoe::OwnMemory;
using bmoe::ResidencyController;
using bmoe::ResidencyEvent;

namespace {

int failures = 0;

void check(bool ok, const char * what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// The 1-based index of the first token the policy calls a stall, or 0 for none.
int first_stall(const std::vector<double> & compute_ms) {
    ResidencyController b;
    for (size_t i = 0; i < compute_ms.size(); ++i)
        if (b.stalled(compute_ms[i] / 1000.0)) return (int) i + 1;
    return 0;
}

} // namespace

int main() {
    // Phone, default budget (4864 MiB held): its own compressed memory jumped at token 26, compute
    // collapsed from 28 on, and the system ended the run at 32. Token 17 was slow with nothing lost.
    const std::vector<double> phone_full = {78,  84,  106, 105, 122, 87,  84,   85,   84,  83,   84,
                                            83,  81,  96,  93,  91,  270, 164,  102,  84,  84,   101,
                                            141, 126, 200, 213, 109, 557, 1210, 3428, 990, 52194};
    // Phone, 814 MiB cache: 48 tokens, no kill.
    const std::vector<double> phone_small = {206, 165, 163, 140, 137, 134, 110, 100, 93,  93,  101, 99,
                                             105, 122, 88,  98,  99,  89,  118, 126, 108, 200, 92,  98,
                                             177, 108, 110, 164, 140, 125, 132, 118, 110, 105, 99,  101,
                                             96,  103, 108, 112, 99,  97,  101, 104, 98,  100, 103, 99};
    // Desktop, 814 MiB cache, steady.
    const std::vector<double> desktop = {160, 150, 140, 135, 120, 118, 125, 122, 119, 121, 117, 123, 120, 119,
                                         124, 118, 122, 121, 120, 125, 119, 118, 123, 122, 120, 121, 119, 124};

    const int s_full = first_stall(phone_full);
    check(s_full == 17, "the phone's slow token 17 is a stall: the residency sample is what tells it from memory");
    {
        ResidencyController b;
        int stalls = 0, first_after_17 = 0;
        for (size_t i = 0; i < phone_full.size(); ++i)
            if (b.stalled(phone_full[i] / 1000.0)) {
                ++stalls;
                if (i + 1 > 17 && !first_after_17) first_after_17 = (int) i + 1;
            }
        check(first_after_17 > 0 && first_after_17 <= 28,
              "the phone's collapse is called by token 28, before the tokens that took seconds");
        check(b.reference() > 0.07 && b.reference() < 0.12,
              "the reference stays at the phone's steady compute, not dragged up by the stalls");
        std::printf("       phone at the default budget: %d stall(s), the first after token 17 at %d\n", stalls,
                    first_after_17);
    }
    check(first_stall(desktop) == 0, "a steady desktop run never stalls, so it never pays for a sample");
    {
        ResidencyController b;
        int stalls = 0;
        for (double c : phone_small)
            stalls += b.stalled(c / 1000.0);
        std::printf("       phone at 814 MiB: %d stall(s)\n", stalls);
        check(stalls <= 2, "the phone's clean run calls at most a couple of stalls, each one only a sample");
    }

    check(ResidencyController::braked(2539u << 20, 581u << 20) == (2539u << 20) / 2, "a brake halves the budget");
    check(ResidencyController::braked(1000u << 20, 581u << 20) == (581u << 20), "never under one token's cycle");
    check(ResidencyController::braked(581u << 20, 581u << 20) == (581u << 20),
          "at the floor there is nothing left to give");

    {
        ResidencyController b;
        bool any = false;
        for (int i = 0; i < 7; ++i)
            any |= b.stalled(i == 6 ? 10.0 : 0.1);
        check(!any && b.reference() == 0.0, "nothing is a stall before the warm-up has set a reference");
    }

    constexpr size_t MiB = 1024 * 1024;
    // Laptop, 16 GB, a 125B model with 4519 MiB of cache and 3470 MiB of dense weights locked: the
    // process's own compressed memory (swap_mib) next to each token's compute. Tokens 35 and 42 are
    // slow with nothing more taken; at token 58 the system took 265 MiB more and the token took 2.5 s.
    const std::vector<double> laptop_ms = {163, 128,  112, 111, 125, 122, 109, 115, 125, 126, 125, 110, 115, 116,
                                           113, 126,  111, 110, 121, 112, 118, 110, 111, 154, 117, 113, 115, 106,
                                           122, 112,  134, 132, 113, 107, 248, 108, 112, 106, 114, 107, 107, 309,
                                           118, 120,  118, 111, 111, 110, 106, 109, 169, 127, 107, 123, 112, 121,
                                           176, 2488, 200, 112, 143, 134, 116, 110, 115, 113, 156, 121, 116, 108};
    const std::vector<double> laptop_lost = {
        609, 609, 609, 609,  609,  609,  609,  609,  608,  638,  639,  639,  639,  642,  680,  699, 708, 745,
        764, 776, 778, 781,  781,  787,  787,  787,  787,  787,  787,  787,  787,  787,  787,  787, 787, 787,
        787, 787, 787, 787,  787,  787,  787,  787,  800,  800,  800,  800,  800,  800,  800,  800, 800, 800,
        800, 800, 800, 1065, 1088, 1088, 1088, 1088, 1088, 1088, 1088, 1088, 1088, 1088, 1088, 1088};
    const size_t laptop_budget = 4519 * MiB, laptop_floor = 633 * MiB;
    {
        ResidencyController r;
        r.start(laptop_budget, laptop_floor);
        size_t budget = laptop_budget;
        int brakes = 0, first_brake = 0, samples = 0;
        for (size_t i = 0; i < laptop_ms.size(); ++i) {
            const OwnMemory own{true, (uint64_t) (laptop_lost[i] * MiB)};
            // The cache was locked: every sampled page is in place.
            budget = r.on_token(laptop_ms[i] / 1000.0, own, budget, [&] {
                ++samples;
                return 0.0;
            });
            if (r.last_event() == ResidencyEvent::brake && !brakes++) first_brake = (int) i + 1;
        }
        std::printf("       laptop, locked cache: %d brake(s), the first at token %d, %d page sample(s)\n", brakes,
                    first_brake, samples);
        check(first_brake == 58, "the laptop brakes at the token where its own memory was taken, not at the slow "
                                 "tokens before it");
        check(brakes == 1, "one brake: the tokens after it are calm, so the episode ends there");
        check(budget == laptop_budget / 2, "the laptop's cache is halved");
        check(r.ceiling() == laptop_budget - laptop_floor, "and the size it braked from is never climbed to again");
        check(samples == 2, "the two slow tokens with nothing taken each cost a page sample, and no budget change");
    }
    {
        // The same trace where the platform does not publish the process's memory: today's behaviour,
        // the page sample alone, which a locked cache answers with "nothing lost".
        ResidencyController r;
        r.start(laptop_budget, laptop_floor);
        size_t budget = laptop_budget;
        for (double c : laptop_ms)
            budget = r.on_token(c / 1000.0, OwnMemory{}, budget, [] { return 0.0; });
        check(budget == laptop_budget, "without the process's own figure a locked cache is never braked");
    }
    {
        // Recovery: after a brake, a calm run climbs back a cycle at a time, every 64 clean tokens,
        // to just under the size it braked from; a second brake lowers that and waits twice as long.
        ResidencyController r;
        r.start(4000 * MiB, 500 * MiB);
        size_t budget = 4000 * MiB;
        auto run = [&](int n, double ms, uint64_t lost_mib, double sample) {
            int recovers = 0;
            for (int i = 0; i < n; ++i) {
                budget = r.on_token(ms / 1000.0, OwnMemory{true, lost_mib * MiB}, budget, [&] { return sample; });
                recovers += r.last_event() == ResidencyEvent::recover;
            }
            return recovers;
        };
        run(32, 100, 100, 0.0);
        run(1, 900, 600, 0.0);
        check(budget == 2000 * MiB && r.ceiling() == 3500 * MiB, "a confirmed stall halves 4000 to 2000, ceiling 3500");
        check(run(63, 100, 600, 0.0) == 0 && budget == 2000 * MiB, "nothing moves before 64 clean tokens");
        check(run(1, 100, 600, 0.0) == 1 && budget == 2500 * MiB, "the 64th clean token climbs one cycle");
        run(64 * 3, 100, 600, 0.0);
        check(budget == 3500 * MiB, "it climbs to the ceiling and stops there, under the size refused");
        run(64, 100, 600, 0.0);
        check(budget == 3500 * MiB, "and never over it");
        run(1, 900, 1200, 0.0);
        check(budget == 1750 * MiB && r.ceiling() == 3000 * MiB, "a second brake halves again and lowers the ceiling");
        check(run(127, 100, 1200, 0.0) == 0, "the second recovery waits 128 clean tokens, not 64");
        check(run(1, 100, 1200, 0.0) == 1, "and then climbs");
        // The system keeps taking memory without stalling the run: no climb.
        const size_t before = budget;
        int climbs = 0;
        for (int i = 0; i < 128; ++i) {
            budget = r.on_token(0.1, OwnMemory{true, (uint64_t) (1200 + 10 * i) * MiB}, budget, [] { return 0.0; });
            climbs += r.last_event() == ResidencyEvent::recover;
        }
        check(climbs == 0 && budget == before, "no climb while the system is still taking memory, stall or not");
    }
    {
        // A machine under no pressure: slow tokens (a neighbour on the cores, a clock step) with
        // nothing taken and every page in place. The plan's budget is never touched.
        ResidencyController r;
        r.start(3000 * MiB, 600 * MiB);
        size_t budget = 3000 * MiB;
        bool moved = false;
        for (int i = 0; i < 2000; ++i) {
            const double ms = (i % 97 == 50) ? 600 : 110 + (i % 7);
            budget = r.on_token(ms / 1000.0, OwnMemory{true, 50 * MiB}, budget, [] { return 0.0; });
            moved |= r.last_event() != ResidencyEvent::none;
        }
        check(!moved && budget == 3000 * MiB, "an unpressed run keeps the planned budget for 2000 tokens");
    }
    {
        // The phone that was ended at the default budget: its own figure was not recorded, so the
        // page sample confirms, from token 26 when its compressed memory jumped.
        ResidencyController r;
        r.start(4864 * MiB, 1051 * MiB);
        size_t budget = 4864 * MiB;
        int first_brake = 0;
        for (size_t i = 0; i < phone_full.size(); ++i) {
            budget = r.on_token(phone_full[i] / 1000.0, OwnMemory{}, budget, [&] { return i + 1 >= 26 ? 0.3 : 0.0; });
            if (r.last_event() == ResidencyEvent::brake && !first_brake) first_brake = (int) i + 1;
        }
        std::printf("       phone at the default budget: first brake at token %d, cache %zu MiB\n", first_brake,
                    budget / MiB);
        check(first_brake > 0 && first_brake <= 28,
              "the phone brakes by token 28, before the tokens that took seconds");
    }

    {
        // A system that keeps taking memory after the first brake (a laptop with 5 GB in active use
        // beside a 9.7 GB engine): the stalls go on, the memory does not come back, so it keeps
        // giving, one halving per stalled token, down to the floor. The ceiling is lowered once, from
        // the size the episode started at, and the recovery climbs back to it once things are calm.
        ResidencyController r;
        r.start(5775 * MiB, 633 * MiB);
        size_t budget = 5775 * MiB;
        auto tok = [&](double ms, uint64_t lost_mib) {
            budget = r.on_token(ms / 1000.0, OwnMemory{true, lost_mib * MiB}, budget, [] { return 0.0; });
            return r.last_event();
        };
        for (int i = 0; i < 40; ++i)
            tok(115, 740);
        int brakes = 0;
        brakes += tok(2635, 1249) == ResidencyEvent::brake;
        for (int i = 0; i < 12; ++i)
            brakes += tok(7000, 1200 - 3 * i) == ResidencyEvent::brake;
        std::printf("       sustained pressure: %d brake(s), cache %zu MiB, ceiling %zu MiB\n", brakes, budget / MiB,
                    r.ceiling() / MiB);
        check(budget == 633 * MiB, "sustained stalls with the memory still gone take the cache to the floor");
        check(r.ceiling() == (5775 - 633) * MiB, "the ceiling is lowered once, from where the episode started");
        int recovers = 0;
        for (int i = 0; i < 64 * 8; ++i)
            recovers += tok(115, 1150) == ResidencyEvent::recover;
        check(recovers == 8 && budget == r.ceiling(),
              "once calm it climbs a cycle every 64 tokens, back to the ceiling");
    }

    return failures ? 1 : 0;
}
