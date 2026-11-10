// The discardable-entries policy against a fake kernel: which entries are offered and when, what a
// lookup finds after a drop, when a drop is measurable, and when offering ahead is priced away.
// No platform, no model; time is a fake clock the fake kernel advances.
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
        // The price of offering ahead: a fake clock the fake kernel advances, as a slow or a cheap
        // offer/reclaim would, with `step` ns of other work between rounds.
        struct Timed : FakeKernel {
            int64_t t = 0, cost = 0;
            bool offer(Span s) override {
                t += cost;
                return FakeKernel::offer(s);
            }
            bool reclaim(Span s) override {
                t += cost;
                return FakeKernel::reclaim(s);
            }
        };
        // One round per generation: entry id%16 used, the entry used offer_after (1) before offered
        // and taken back, so every round is a kept round trip.
        const auto rounds = [&](DiscardableEntries & d, Timed & k, int64_t step, int n, uint32_t & gen,
                                bool priced = true) {
            bool moved = false;
            for (int i = 0; i < n; ++i, ++gen) {
                const int32_t id = (int32_t) (gen % 16);
                d.used(id, gen);
                moved = d.offer_cold(gen, all_valid, priced) || moved;
                const int32_t back = (int32_t) ((gen + 15) % 16);
                if (d.offered(back)) d.take_back(back);
                k.t += step;
            }
            return moved;
        };
        const long long w = DiscardableEntries::price_window_round_trips;
        {
            Timed k;
            k.cost = 1; // cheap: 4 ns of mechanism per round against 1000 of other work
            DiscardableEntries d;
            d.init(16, 1, false, &k, two_spans());
            d.set_cost_rule(0.05, 1, [&] { return k.t; });
            uint32_t gen = 0;
            rounds(d, k, 1000, (int) (4 * w), gen);
            check(!d.pressure_only() && d.cost_share() >= 0 && d.cost_share() < 0.05,
                  "a cheap mechanism is measured and keeps offering ahead");
        }
        {
            Timed k;
            k.cost = 200; // dear: 800 ns per round against 1000
            DiscardableEntries d;
            d.init(16, 1, false, &k, two_spans());
            d.set_cost_rule(0.05, 1, [&] { return k.t; });
            uint32_t gen = 0;
            check(!rounds(d, k, 1000, (int) w - 2, gen) && !d.pressure_only(),
                  "nothing is decided before a window of round trips");
            check(rounds(d, k, 1000, 4, gen) && d.pressure_only() && d.cost_share() > 0.05,
                  "a dear mechanism turns offering pressure-only, said once");
            const int offers = k.offers;
            check(!rounds(d, k, 1000, 32, gen) && k.offers == offers && d.offered_bytes() == 0,
                  "pressure-only offers nothing while the system is not short");
            d.set_shortfall(3); // each entry offers two 1-byte spans: two entries cover it
            rounds(d, k, 1000, 1, gen);
            check(d.offered_bytes() >= 3 && d.offered_bytes() <= 4, "a shortfall is covered, and no more");
            // The last round used entry (gen+15)%16; the oldest live use is entry gen%16.
            check(d.offered((int32_t) (gen % 16)) && !d.offered((int32_t) ((gen + 15) % 16)),
                  "the coldest entries go first, the newest stays ordinary");
            d.set_shortfall(0);
        }
        {
            // A prompt cycles entries far faster than decode: dear there, cheap in decode. Only the
            // decode is judged (measured on the phone: 21-23% when the prompt was priced, ~6% not).
            Timed k;
            k.cost = 200;
            DiscardableEntries d;
            d.init(16, 1, false, &k, two_spans());
            d.set_cost_rule(0.05, 1, [&] { return k.t; });
            uint32_t gen = 0;
            rounds(d, k, 10, (int) (4 * w), gen, false);
            check(!d.pressure_only() && d.cost_share() < 0, "an unpriced load (a prompt) is never judged");
            k.cost = 1;
            rounds(d, k, 1000, (int) (4 * w), gen);
            check(!d.pressure_only() && d.cost_share() >= 0 && d.cost_share() < 0.05,
                  "and the decode after it is judged on its own cost");
        }
        {
            Timed k;
            DiscardableEntries d;
            d.init(16, 1, false, &k, two_spans());
            d.set_cost_rule(0.05, 4 * (uint32_t) w, [&] { return k.t; });
            k.cost = 200;
            uint32_t gen = 0;
            rounds(d, k, 1000, (int) (2 * w), gen);
            check(!d.pressure_only(), "nothing is judged before one token's loads, whatever the round trips");
            rounds(d, k, 1000, (int) (3 * w), gen);
            check(d.pressure_only(), "a plain overrun is judged after one token");
        }
        {
            // Near the cap (8% against 5%): judged only once the window spans its tokens.
            Timed k;
            k.cost = 22; // 88 ns per round against 1000 of other work
            DiscardableEntries d;
            d.init(16, 1, false, &k, two_spans());
            const uint32_t per_token = 16;
            d.set_cost_rule(0.05, per_token, [&] { return k.t; });
            uint32_t gen = 0;
            rounds(d, k, 1000, 80, gen);
            check(!d.pressure_only() && d.cost_share() < 0, "a share near the cap waits for the whole window");
            rounds(d, k, 1000, (int) (per_token * DiscardableEntries::price_window_tokens), gen);
            check(d.pressure_only() && d.cost_share() > 0.05 && d.cost_share() < 0.1, "and is judged on it");
        }
        {
            Timed k;
            k.cost = 1000000; // ruinous, but the gates' drop-before-reclaim must still reach every offer
            DiscardableEntries d;
            d.init(16, 1, true, &k, two_spans());
            d.set_cost_rule(0.05, 1, [&] { return k.t; });
            uint32_t gen = 0;
            rounds(d, k, 1000, (int) (2 * w), gen);
            check(!d.pressure_only(), "drop-before-reclaim is never priced away");
        }
        {
            Timed k;
            DiscardableEntries d;
            d.init(16, 1, false, &k, two_spans());
            d.set_cost_rule(0.0, 1, [&] { return k.t; });
            uint32_t gen = 0;
            rounds(d, k, 1000, 8, gen);
            check(d.pressure_only() && k.offers == 0, "a zero share is pressure-only from the start");
        }
        {
            Timed k;
            k.cost = 1000000;
            DiscardableEntries d;
            d.init(16, 1, false, &k, two_spans());
            d.set_cost_rule(-1.0, 1, [&] { return k.t; });
            uint32_t gen = 0;
            rounds(d, k, 1000, (int) (2 * w), gen);
            check(!d.pressure_only() && d.cost_share() < 0, "an unpriced rule always offers ahead");
        }
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
