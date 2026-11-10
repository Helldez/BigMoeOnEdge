#include "discardable_entries.h"

#include "../io/platform_io.h"

#include <algorithm>

namespace bmoe {

namespace {
class PlatformSpanOps final : public SpanOps {
public:
    bool supported() const override { return pio::vm_discard_supported(); }
    bool prepare(Span s) override { return pio::vm_commit_discardable(s.p, s.size); }
    bool offer(Span s) override { return pio::vm_offer(s.p, s.size); }
    bool reclaim(Span s) override { return pio::vm_reclaim(s.p, s.size); }
    int held(Span s) override { return pio::vm_offered_held(s.p, s.size); }
    void drop(Span s) override { pio::vm_evict(s.p, s.size); }
};
} // namespace

SpanOps & platform_span_ops() {
    static PlatformSpanOps ops;
    return ops;
}

void DiscardableEntries::init(
    size_t n_entry, uint32_t offer_after, bool drop_before_reclaim, SpanOps * ops, SpansOf spans) {
    ops_ = ops && ops->supported() ? ops : nullptr;
    spans_ = std::move(spans);
    offer_after_ = offer_after;
    drop_before_reclaim_ = drop_before_reclaim;
    offered_.assign(enabled() ? n_entry : 0, 0);
    last_use_.assign(enabled() ? n_entry : 0, 0);
    uses_.clear();
    lost_on_lookup_ = 0;
}

void DiscardableEntries::used(int32_t id, uint32_t gen) {
    if (!enabled()) return;
    last_use_[id] = gen;
    uses_.emplace_back(id, gen);
}

void DiscardableEntries::offer_cold(uint32_t gen, const std::function<bool(int32_t)> & valid) {
    if (!enabled()) return;
    while (!uses_.empty() && uses_.front().second + offer_after_ <= gen) {
        const auto [id, g] = uses_.front();
        uses_.pop_front();
        if (offered_[id] || last_use_[id] != g || !valid(id)) continue;
        bool any = false;
        spans_(id, [&](Span s) { any = ops_->offer(s) || any; });
        offered_[id] = any ? 1 : 0;
    }
}

bool DiscardableEntries::take_back(int32_t id) {
    if (!offered(id)) return true;
    offered_[id] = 0;
    bool intact = true;
    spans_(id, [&](Span s) {
        if (drop_before_reclaim_) ops_->drop(s);
        intact = ops_->reclaim(s) && intact; // every span, not short-circuited: all must come back
    });
    if (intact) return true;
    // Lost: make every span offerable again (and committed, where a drop decommits), so the owner's
    // refill lands in memory the next offer can hand over like any other.
    spans_(id, [&](Span s) { ops_->prepare(s); });
    ++lost_on_lookup_;
    return false;
}

bool DiscardableEntries::dropped(int32_t id) {
    if (!offered(id)) return false;
    int verdict = -1;
    spans_(id, [&](Span s) {
        if (verdict == -1) verdict = ops_->held(s);
    });
    return verdict == 0;
}

void DropFollowingBudget::reset(size_t budget, size_t floor, uint32_t window) {
    ceiling_ = budget;
    current_ = budget;
    window_ = window;
    dropped_ever_ = false;
    set_floor(floor);
}

void DropFollowingBudget::set_floor(size_t floor) {
    floor_ = floor > 0 ? floor : 1;
}

void DropFollowingBudget::dropped(size_t bytes, uint32_t gen) {
    last_drop_ = gen;
    dropped_ever_ = true;
    if (current_ > floor_) current_ = std::max(floor_, current_ > bytes ? current_ - bytes : 0);
}

size_t DropFollowingBudget::make_room(size_t need, uint32_t gen) {
    const bool quiet = !dropped_ever_ || gen - last_drop_ >= window_;
    if (quiet && current_ < ceiling_) current_ = std::min(ceiling_, current_ + need);
    return current_;
}

} // namespace bmoe
