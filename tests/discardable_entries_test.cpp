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
    return fails ? 1 : 0;
}
