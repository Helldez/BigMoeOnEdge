#include "discardable_entries.h"

#include "../io/platform_io.h"

namespace bmoe {

namespace {
class PlatformSpanOps final : public SpanOps {
public:
    bool supported() const override { return pio::vm_discard_supported(); }
    bool prepare(Span s) override { return pio::vm_commit_discardable(s.p, s.size); }
    bool offer(Span s) override { return pio::vm_offer(s.p, s.size); }
    bool reclaim(Span s) override { return pio::vm_reclaim(s.p, s.size); }
    int held(Span s) override {
        // One page tells for the span: the kernel drops an offered span whole on every platform
        // that reports it, and a page is what mincore answers about.
        size_t sampled = 0, resident = 0;
        if (!pio::vm_resident_sample(s.p, pio::vm_page(), &sampled, &resident) || sampled == 0) return -1;
        return resident != 0 ? 1 : 0;
    }
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

} // namespace bmoe
