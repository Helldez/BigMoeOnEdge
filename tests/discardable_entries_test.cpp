// The discardable-entries policy against a fake kernel: which entries are offered and when, what a
// lookup finds after a drop, and when a drop is measurable. No platform, no model, no timing.
#include "discardable_entries.h"

#include <cstdio>
#include <map>

using namespace bmoe;

namespace {
int fails = 0;
void check(bool ok, const char * what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

// A kernel the test controls: a span is held, offered, or dropped; `measurable` says whether this
// platform can tell a dropped span from a held one.
struct FakeKernel : SpanOps {
    enum State { Held, Offered, Dropped };
    std::map<void *, State> state;
    bool measurable = true;
    int offers = 0, reclaims = 0, prepares = 0;
    bool supported() const override { return true; }
    bool prepare(Span s) override {
        ++prepares;
        state[s.p] = Held;
        return true;
    }
    bool offer(Span s) override {
        ++offers;
        state[s.p] = Offered;
        return true;
    }
    bool reclaim(Span s) override {
        ++reclaims;
        const bool intact = state[s.p] != Dropped;
        state[s.p] = Held;
        return intact;
    }
    int held(Span s) override { return measurable ? (state[s.p] == Dropped ? 0 : 1) : -1; }
    void drop(Span s) override { state[s.p] = Dropped; }
    void pressure_takes(void * p) { state[p] = Dropped; }
};

// Two spans per entry, addressed by entry id, as an expert's projections would be.
char arena[16][2];
DiscardableEntries::SpansOf two_spans() {
    return [](int32_t id, const std::function<void(Span)> & fn) {
        fn(Span{&arena[id][0], 1});
        fn(Span{&arena[id][1], 1});
    };
}
} // namespace

int main() {
    const auto all_valid = [](int32_t) { return true; };
    {
        FakeKernel k;
        DiscardableEntries d;
        d.init(16, 3, false, &k, two_spans());
        d.used(1, 10);
        d.used(2, 11);
        d.offer_cold(12, all_valid);
        check(!d.offered(1) && !d.offered(2), "nothing is offered before its last use leaves the window");
        d.offer_cold(13, all_valid);
        check(d.offered(1) && !d.offered(2), "an entry is offered when its last use is offer_after old");
        check(k.offers == 2, "every span of the entry is offered");

        d.used(2, 12); // used again: the pair (2, 11) is stale
        d.offer_cold(14, all_valid);
        check(!d.offered(2), "an entry used again is not offered on its older use");
        d.offer_cold(15, all_valid);
        check(d.offered(2), "and is offered when the newer use leaves the window");

        check(d.take_back(1) && !d.offered(1), "a held offer comes back intact and is ordinary again");
        k.pressure_takes(&arena[2][1]);
        check(d.dropped(2) == false, "a drop is measured on the first span only");
        k.pressure_takes(&arena[2][0]);
        check(d.dropped(2), "a drop the platform can see is reported");
        const int prepares = k.prepares;
        check(!d.take_back(2) && d.lost_on_lookup() == 1, "a lookup on a dropped entry reports it lost");
        check(k.prepares == prepares + 2, "a lost entry's spans are prepared again for the refill");
        check(!d.offered(2) && !d.dropped(2), "and the entry is ordinary until it goes cold again");
    }
    {
        FakeKernel k;
        DiscardableEntries d;
        d.init(16, 1, false, &k, two_spans());
        d.used(5, 0);
        d.offer_cold(1, [](int32_t id) { return id != 5; });
        check(!d.offered(5), "an entry the owner no longer holds is never offered");
        d.used(6, 1);
        d.offer_cold(2, all_valid);
        d.forget(6);
        check(!d.offered(6), "forget clears an offer whose memory the owner released");
    }
    {
        FakeKernel k;
        k.measurable = false;
        DiscardableEntries d;
        d.init(16, 1, false, &k, two_spans());
        d.used(3, 0);
        d.offer_cold(1, all_valid);
        k.pressure_takes(&arena[3][0]);
        check(!d.dropped(3), "where residency cannot be measured, nothing is reported dropped");
        check(!d.take_back(3), "and the lookup still finds the loss: correctness does not rest on measuring");
    }
    {
        FakeKernel k;
        DiscardableEntries d;
        d.init(16, 1, true, &k, two_spans());
        d.used(4, 0);
        d.offer_cold(1, all_valid);
        check(!d.take_back(4), "drop-before-reclaim (the gates' pressure) makes every offered lookup a loss");
    }
    {
        DiscardableEntries d;
        d.init(16, 1, false, nullptr, two_spans());
        d.used(1, 0);
        d.offer_cold(5, all_valid);
        check(!d.enabled() && !d.offered(1) && d.take_back(1), "without a mechanism every call is inert");
    }
    {
        DropFollowingBudget b;
        b.reset(1000, 100, 10);
        check(b.make_room(50, 0) == 1000, "never past the budget the owner set");
        b.dropped(300, 20);
        check(b.current() == 700, "a drop lowers the budget by what the kernel took");
        b.dropped(5000, 21);
        check(b.current() == 100, "never under the floor (one token's cycle)");
        check(b.make_room(50, 25) == 100, "no growth while the kernel took something within the window");
        check(b.make_room(50, 31) == 150, "after a quiet window it grows by what a load needs");
        check(b.make_room(5000, 32) == 1000, "and stops at the ceiling");
        b.dropped(400, 40);
        b.reset(500, 100, 10);
        check(b.current() == 500 && b.make_room(10, 41) == 500,
              "an explicit resize (the brake's) is the new ceiling, and a fresh start");
        b.set_floor(600);
        b.dropped(10, 50);
        check(b.current() == 500, "a drop at or under the floor leaves the budget where it is");
    }
    {
        FakeKernel k;
        DiscardableEntries d;
        d.init(16, 3, false, &k, two_spans());
        d.offer_cold(100, all_valid, 0);
        check(d.window() == 3, "the window starts at offer_after");
        d.offer_cold(103, all_valid, 0);
        d.offer_cold(109, all_valid, 0);
        check(d.window() == 12, "it doubles each time it passes whole without a drop");
        d.offer_cold(121, all_valid, 0);
        d.offer_cold(145, all_valid, 0);
        d.offer_cold(193, all_valid, 0);
        d.offer_cold(289, all_valid, 0);
        check(d.window() == 144, "and never covers more than the younger half of the ages the cache holds");
        d.offer_cold(290, all_valid, 280);
        check(d.window() == 5, "a younger cache narrows it at once");
    }
    {
        FakeKernel k;
        DiscardableEntries d;
        d.init(16, 3, false, &k, two_spans());
        d.used(1, 0);
        d.offer_cold(10, all_valid, 0);
        check(d.offered(1), "an entry past the window is offered");
        d.offer_cold(13, all_valid, 0);
        d.offer_cold(19, all_valid, 0);
        check(d.window() == 9, "quiet: the window widened");
        k.pressure_takes(&arena[1][0]);
        check(!d.take_back(1), "the kernel took the entry");
        d.offer_cold(20, all_valid, 0);
        check(d.window() == 3, "a drop returns the window to offer_after");
        d.offer_cold(22, all_valid, 0);
        check(d.window() == 3, "and the quiet stretch restarts from the drop");
        d.offer_cold(25, all_valid, 0);
        d.offer_cold(31, all_valid, 0);
        check(d.window() == 12, "quiet again: it widens again");
        d.pressure();
        d.offer_cold(32, all_valid, 0);
        check(d.window() == 3, "the owner's measured pressure (the brake) narrows it as a drop does");
    }
    {
        FakeKernel k;
        DiscardableEntries d;
        d.init(16, 3, false, &k, two_spans());
        d.offer_cold(100, all_valid);
        d.offer_cold(200, all_valid);
        check(d.window() == 3, "without the owner's oldest use the window stays at offer_after");
    }
    {
        DropFollowingBudget b;
        b.reset(1000, 100, 10);
        b.braked(500, 0);
        check(b.current() == 500 && b.ceiling() == 1000, "a brake lowers the budget and keeps the owner's ceiling");
        check(b.make_room(100, 15) == 500, "after a brake, one quiet window is not enough");
        check(b.make_room(100, 20) == 600, "two quiet windows after the first brake, it climbs back");
        b.braked(300, 30);
        check(b.make_room(100, 69) == 300, "a second brake doubles the quiet stretch again");
        check(b.make_room(100, 70) == 400, "and after it the cache climbs once more");
        check(b.make_room(5000, 71) == 1000, "never past the owner's ceiling");
        b.braked(50, 80);
        check(b.current() == 100, "a brake never takes the budget under the floor");
    }
    {
        DropFollowingBudget b;
        b.reset(1000, 100, 10);
        b.lend(300);
        check(b.current() == 700 && b.ceiling() == 1000, "a loan lowers the budget, not the ceiling");
        check(b.make_room(500, 5) == 700, "nothing grows into a loan");
        b.dropped(200, 6);
        b.end_loan();
        check(b.current() == 800, "the loan comes back on top of what the kernel left");
        check(b.make_room(500, 16) == 1000, "and a loan is no pressure: growth waits one window, as after a drop");
        b.reset(1000, 100, 10);
        b.lend(5000);
        check(b.current() == 1, "a loan larger than the cache leaves it one byte, as the shared-slot guard needs");
        b.end_loan();
        check(b.current() == 1000, "and returns it whole");
    }
    return fails ? 1 : 0;
}
