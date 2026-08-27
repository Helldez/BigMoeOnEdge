// Unit tests for StallUnionState (core/src/moe/stall_union.h) — the interval-union state machine.
//
// The tests drive the pure state machine with injected timestamps: the production StallUnion is
// this class plus a mutex and an in-critical-section clock read, so what needs exhaustive
// testing is the arithmetic — overlap, nesting, separation, the 0→1/1→0 boundaries and the
// open-interval snapshot rule — all deterministic, with no wall clock and no scheduling luck.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "stall_union.h"

#include <cstdio>

using namespace bmoe;

static int failures = 0;

static void check(const char * what, long long got, long long want) {
    const bool ok = got == want;
    if (!ok) ++failures;
    std::printf("%-44s got %lld  want %lld  %s\n", what, got, want, ok ? "ok" : "FAIL");
}

int main() {
    // Case A — one wait: the interval is its length, whole and nothing else.
    {
        StallUnionState u;
        u.enter_at(10);
        check("A: one wait [10,30)", u.total_at(29), 19); // open interval, snapshotted mid-stall
        u.exit_at(30);
        check("A: one wait [10,30)", u.total_at(100), 20);
    }

    // Case B — a wait nested inside another counts once: union 20, not 40.
    {
        StallUnionState u;
        u.enter_at(10);
        u.enter_at(15);
        u.exit_at(25);
        check("B: nested snapshot [10,25)", u.total_at(25), 15);
        u.exit_at(30);
        check("B: nested waits", u.total_at(100), 20);
    }

    // Case C — partial overlap abuts into one interval: [10,30)∪[20,40) = 30.
    {
        StallUnionState u;
        u.enter_at(10);
        u.enter_at(20);
        u.exit_at(30);
        u.exit_at(40);
        check("C: partial overlap", u.total_at(100), 30);
    }

    // Case D — separated intervals both count: [10,20) + [30,50) = 30.
    {
        StallUnionState u;
        u.enter_at(10);
        u.exit_at(20);
        u.enter_at(30);
        u.exit_at(50);
        check("D: separated intervals", u.total_at(100), 30);
    }

    // Case E — three waits, two nested in the first: [10,50)∪[20,30)∪[25,45) = 40.
    {
        StallUnionState u;
        u.enter_at(10);
        u.enter_at(20);
        u.enter_at(25);
        u.exit_at(30);
        u.exit_at(45);
        check("E: snapshot, one still open", u.total_at(45), 35);
        u.exit_at(50);
        check("E: triple, nested", u.total_at(100), 40);
    }

    // Case F — the boundaries: a second enter while the interval is open must not restart it, and
    // only the 1->0 exit may close it. Interleave a third thread to exercise 2->3->2.
    {
        StallUnionState u;
        u.enter_at(100); // 0 -> 1: opens
        u.enter_at(110); // 1 -> 2: no restart
        u.enter_at(120); // 2 -> 3
        u.exit_at(130);  // 3 -> 2
        check("F: mid-flight snapshot", u.total_at(130), 30);
        u.exit_at(999); // 2 -> 1: must NOT close — a bogus close would stop at 899 here
        check("F: still open after 2->1", u.total_at(999), 899);
        u.exit_at(200); // 1 -> 0: closes at 200
        check("F: closed at 1->0", u.total_at(1000), 100);
    }

    // The stats() rule: a snapshot taken while stalled includes the open interval up to now, so a
    // per-token delta spanning it cannot lose the part already elapsed.
    {
        StallUnionState u;
        u.enter_at(1000);
        const long long first = u.total_at(1500); // 500 elapsed
        u.exit_at(2000);
        const long long second = u.total_at(2000) - first;
        check("snapshot deltas sum to the union", first + second, 1000);
    }

    // reset() zeroes everything, interval included.
    {
        StallUnionState u;
        u.enter_at(10);
        u.exit_at(30);
        u.enter_at(40);
        u.reset();
        check("reset clears total", u.total_at(100), 0);
        u.exit_at(50); // stray exit on an empty state: refused, count not driven negative
        check("exit on empty is inert", u.total_at(100), 0);
        // ...and the refusal is what keeps the NEXT interval honest: without the guard the count
        // sits at -1, the following enter reads as a "close" of a garbage interval, and every
        // number after it is wrong.
        u.enter_at(1000);
        u.exit_at(1050);
        check("interval after stray exit is correct", u.total_at(2000), 50);
    }

    if (failures) {
        std::printf("stall_union_test: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("stall_union_test: all ok\n");
    return 0;
}
