// The cache brake's policy, replayed on per-token compute traces measured on real machines.
//
// The traces are the `compute_ms` column of runs that were kept: a 12 GB phone whose system took the
// engine's memory back at the default budget and ended the run, the same phone at a small budget that
// ran clean, and a 16 GB desktop at the same small budget. What is checked is when the policy calls a
// stall. Whether a stall moves the budget is the residency sample's to say, which no trace can replay.

#include "../core/src/engine/cache_brake.h"

#include <cstdio>
#include <vector>

using bmoe::CacheBrake;

namespace {

int failures = 0;

void check(bool ok, const char * what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// The 1-based index of the first token the policy calls a stall, or 0 for none.
int first_stall(const std::vector<double> & compute_ms) {
    CacheBrake b;
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
        CacheBrake b;
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
        CacheBrake b;
        int stalls = 0;
        for (double c : phone_small)
            stalls += b.stalled(c / 1000.0);
        std::printf("       phone at 814 MiB: %d stall(s)\n", stalls);
        check(stalls <= 2, "the phone's clean run calls at most a couple of stalls, each one only a sample");
    }

    check(CacheBrake::braked(2539u << 20, 581u << 20) == (2539u << 20) / 2, "a brake halves the budget");
    check(CacheBrake::braked(1000u << 20, 581u << 20) == (581u << 20), "never under one token's cycle");
    check(CacheBrake::braked(581u << 20, 581u << 20) == (581u << 20), "at the floor there is nothing left to give");

    {
        CacheBrake b;
        bool any = false;
        for (int i = 0; i < 7; ++i)
            any |= b.stalled(i == 6 ? 10.0 : 0.1);
        check(!any && b.reference() == 0.0, "nothing is a stall before the warm-up has set a reference");
    }

    return failures ? 1 : 0;
}
