// Discardable cache entries: which cached entries are handed to the kernel to DROP under memory
// pressure, and what became of them.
//
// Memory the kernel may only reclaim by writing it somewhere (swap, or swapping everything else
// around memory that is locked) costs the drive a write. Memory it may drop costs nothing to reclaim
// and one read to restore, and a cache of bytes that already sit in the model file is exactly that.
// This class is the policy; it names no platform. The mechanism is a SpanOps port, implemented over
// pio's offer/reclaim calls (platform_span_ops) and by a fake in the tests. The owner of the cache
// keeps everything else - the entries, their LRU, the budget - and says which spans an entry owns.
//
// The rule is the cache's own: an entry whose last use is `offer_after` generations old is offered;
// a lookup takes an offered entry back before anything reads it, and an entry the kernel dropped is
// a miss. The owner picks offer_after as the oldest an entry can be while a kernel or an I/O lane
// may still be using it.
//
// Offering ahead of pressure is insurance, and its premium is what the mechanism costs when the
// kernel takes nothing: an offer and a reclaim per reuse. That premium is not a property of the
// policy but of the machine, and machines differ by more than an order of magnitude (measured per
// 1.7 MiB entry: ~34 us on an Android phone, 1-2 ms on a Windows PC, where it was 40% of decode).
// So the policy prices it as it runs: it times its own offers and reclaims against the wall time
// they happen in, and once a window of round trips costs more than the share its owner allows,
// offering turns pressure-only for the rest of the run: nothing is offered until the owner reports
// the system short of memory, and then the coldest entries, as many bytes as it is short.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

namespace bmoe {

// One page-aligned span of memory that no other entry shares a page with.
struct Span {
    void * p = nullptr;
    size_t size = 0;
};

// The mechanism, per span. See pio::vm_offer for what each platform does underneath.
class SpanOps {
public:
    virtual ~SpanOps() = default;
    virtual bool supported() const = 0;
    // Make a just-committed span offerable later (on some platforms it must become its own object).
    virtual bool prepare(Span s) = 0;
    virtual bool offer(Span s) = 0;
    // Before anything reads an offered span: true when every byte is still what was offered.
    virtual bool reclaim(Span s) = 0;
    // Whether an offered span is still held: 1 held, 0 dropped, -1 cannot tell on this platform.
    virtual int held(Span s) = 0;
    // Drop a span as pressure would (the gates' stand-in for pressure).
    virtual void drop(Span s) = 0;
};

// pio's implementation of the port.
SpanOps & platform_span_ops();

class DiscardableEntries {
public:
    // Calls `fn` for every span entry `id` owns. Called on the owner's thread only.
    using SpansOf = std::function<void(int32_t id, const std::function<void(Span)> & fn)>;

    // `drop_before_reclaim` (debug): drop an offered entry just before a lookup takes it back, so
    // every such lookup finds it gone and the owner's re-read path runs on zeroed memory.
    void init(size_t n_entry, uint32_t offer_after, bool drop_before_reclaim, SpanOps * ops, SpansOf spans);
    bool enabled() const { return ops_ != nullptr; }

    // Nanoseconds on a monotonic clock. The tests pass a fake one.
    using Clock = std::function<int64_t()>;
    // Price offering ahead of pressure (see the top of this file): `max_share` is the most of the
    // wall time the round trips may take, in [0, 1]; 0 offers only under pressure from the start, and
    // a negative share never prices (offering ahead always). `loads_per_token` is how many priced
    // calls of offer_cold (the owner's loads) make one decode token. Off while drop_before_reclaim
    // is set: the gates' pressure must reach every offer. Call after init.
    void set_cost_rule(double max_share, uint32_t loads_per_token, Clock now_ns = {});
    // Round trips a window holds before it is priced: enough that one slow call cannot decide.
    static constexpr long long price_window_round_trips = 64;
    // Decode tokens a window spans before a share near the cap is judged.
    static constexpr uint32_t price_window_tokens = 8;
    // A single token whose share is past this many times the cap is judged at once. The verdict is
    // asymmetric on purpose: pressure-only by mistake costs what offering ahead would have saved,
    // offering ahead by mistake costs its whole share for every token (40% of decode on the PC),
    // and a run of 24 tokens spent half of itself learning that over eight.
    static constexpr double price_fast_factor = 2.0;
    // The system is short of `bytes` of memory (0: not short). Read only once offering is
    // pressure-only; the owner need not measure before.
    void set_shortfall(size_t bytes) { shortfall_ = bytes; }
    bool pressure_only() const { return pressure_only_; }
    // The share of wall time the last priced window spent offering and reclaiming; -1 before one.
    double cost_share() const { return cost_share_; }

    bool prepare(Span s) { return !ops_ || ops_->prepare(s); }

    // The entry was used (looked up, filled, or prefetched) in generation `gen`.
    void used(int32_t id, uint32_t gen);

    // Offer every entry whose last use is offer_after generations behind `gen` (pressure-only: the
    // coldest of them, while the bytes on offer are short of the shortfall). `valid(id)` says
    // whether the owner still holds the entry; one it no longer does is skipped. `priced` says
    // whether the load this call opens is the kind the cost is judged on: the owner's decode, not a
    // prompt, whose wide loads cycle entries far faster than any token and would price the
    // mechanism at several times its decode cost (measured on the phone: 21-23% against ~6%). The
    // time from one priced call to the next, and the offers and reclaims in it, are the window's;
    // an unpriced call ends that span, so the pause before a prompt is not counted either. True on
    // the call that turned offering pressure-only, so the owner can say so once.
    bool offer_cold(uint32_t gen, const std::function<bool(int32_t)> & valid, bool priced = true);

    bool offered(int32_t id) const { return enabled() && offered_[id] != 0; }

    // Take an offered entry back. False when the kernel dropped any of it: the entry's spans are
    // ordinary memory again and the owner must fill them before anything reads them.
    bool take_back(int32_t id);

    // Whether an offered entry is measurably gone (false when held, or when the platform cannot tell).
    bool dropped(int32_t id);

    // The owner released the entry's memory: whatever was offered there is gone with it.
    void forget(int32_t id);

    long long lost_on_lookup() const { return lost_on_lookup_; }
    size_t offered_bytes() const { return offered_total_; }

private:
    void offer_entry(int32_t id);
    void price_window();
    int64_t now() const;

    SpanOps * ops_ = nullptr;
    SpansOf spans_;
    uint32_t offer_after_ = 0;
    bool drop_before_reclaim_ = false;
    std::vector<uint8_t> offered_;
    std::vector<uint32_t> last_use_;
    std::vector<size_t> offered_size_; // bytes each offered entry put on offer
    // Every use in order, (id, generation): an entry is offered when its LAST use leaves the window,
    // and an older pair for an entry used since is stale and skipped. Amortized O(1) per use.
    std::deque<std::pair<int32_t, uint32_t>> uses_;
    // Pressure-only: the uses that left the window unoffered, coldest first, waiting for a shortfall.
    std::deque<std::pair<int32_t, uint32_t>> cold_;
    long long lost_on_lookup_ = 0;
    size_t offered_total_ = 0, shortfall_ = 0;

    // The price of offering ahead, measured over windows of round trips.
    double max_share_ = -1.0;
    Clock clock_;
    bool pressure_only_ = false;
    double cost_share_ = -1.0;
    bool timing_ = false;  // the span since the last offer_cold is a priced one
    int64_t span_t0_ = -1; // when that span began
    uint32_t loads_per_token_ = 1, window_loads_seen_ = 0;
    int64_t window_wall_ns_ = 0, window_cost_ns_ = 0;
    long long window_round_trips_ = 0;
};

// The budget of a cache the kernel may drop from. Down by what the kernel takes, so a cache larger
// than the machine lets it keep stops refilling only to be dropped again; back up - never past the
// budget its owner set - once a whole window passes without a drop, and only as fast as misses need
// room. Probing upward is safe here as it is not for ordinary memory: growing into pressure costs
// the kernel a drop and the cache a re-read, never a write.
class DropFollowingBudget {
public:
    // The owner set the budget (at load, or an explicit resize such as the brake's): the new
    // ceiling, and where the budget starts. `window` is how many generations without a drop
    // count as the kernel having stopped taking.
    void reset(size_t budget, size_t floor, uint32_t window);
    void set_floor(size_t floor);
    size_t current() const { return current_; }

    // The kernel dropped `bytes` in generation `gen`.
    void dropped(size_t bytes, uint32_t gen);

    // A load needs `need` bytes beyond the budget in generation `gen`: the budget grows by up to
    // that when the kernel has been quiet for a window, and not past the ceiling.
    size_t make_room(size_t need, uint32_t gen);

private:
    size_t ceiling_ = 0, floor_ = 1, current_ = 0;
    uint32_t window_ = 0, last_drop_ = 0;
    bool dropped_ever_ = false;
};

} // namespace bmoe
