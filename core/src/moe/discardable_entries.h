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
// The rule is the cache's own and needs no tuning: an entry whose last use is `offer_after`
// generations old is offered; a lookup takes an offered entry back before anything reads it, and an
// entry the kernel dropped is a miss. The owner picks offer_after as the oldest an entry can be
// while a kernel or an I/O lane may still be using it.
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

    bool prepare(Span s) { return !ops_ || ops_->prepare(s); }

    // The entry was used (looked up, filled, or prefetched) in generation `gen`.
    void used(int32_t id, uint32_t gen);

    // Offer every entry whose last use is offer_after generations behind `gen`. `valid(id)` says
    // whether the owner still holds the entry; one it no longer does is skipped.
    void offer_cold(uint32_t gen, const std::function<bool(int32_t)> & valid);

    bool offered(int32_t id) const { return enabled() && offered_[id] != 0; }

    // Take an offered entry back. False when the kernel dropped any of it: the entry's spans are
    // ordinary memory again and the owner must fill them before anything reads them.
    bool take_back(int32_t id);

    // Whether an offered entry is measurably gone (false when held, or when the platform cannot tell).
    bool dropped(int32_t id);

    // The owner released the entry's memory: whatever was offered there is gone with it.
    void forget(int32_t id) {
        if (enabled()) offered_[id] = 0;
    }

    long long lost_on_lookup() const { return lost_on_lookup_; }

private:
    SpanOps * ops_ = nullptr;
    SpansOf spans_;
    uint32_t offer_after_ = 0;
    bool drop_before_reclaim_ = false;
    std::vector<uint8_t> offered_;
    std::vector<uint32_t> last_use_;
    // Every use in order, (id, generation): an entry is offered when its LAST use leaves the window,
    // and an older pair for an entry used since is stale and skipped. Amortized O(1) per use.
    std::deque<std::pair<int32_t, uint32_t>> uses_;
    long long lost_on_lookup_ = 0;
};

} // namespace bmoe
