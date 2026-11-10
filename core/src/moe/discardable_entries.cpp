#include "discardable_entries.h"

#include "../io/platform_io.h"

#include <algorithm>
#include <chrono>

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
    offered_size_.assign(enabled() ? n_entry : 0, 0);
    uses_.clear();
    cold_.clear();
    lost_on_lookup_ = 0;
    offered_total_ = shortfall_ = 0;
    max_share_ = -1.0;
    pressure_only_ = false;
    cost_share_ = -1.0;
    timing_ = false;
    span_t0_ = -1;
    loads_per_token_ = 1;
    window_loads_seen_ = 0;
    window_wall_ns_ = window_cost_ns_ = 0;
    window_round_trips_ = 0;
}

void DiscardableEntries::set_cost_rule(double max_share, uint32_t loads_per_token, Clock now_ns) {
    // The gates' drop-before-reclaim is pressure on every offer: pricing it away would hide the path
    // they exist to run.
    max_share_ = drop_before_reclaim_ ? -1.0 : std::min(max_share, 1.0);
    loads_per_token_ = std::max<uint32_t>(1, loads_per_token);
    clock_ = std::move(now_ns);
    pressure_only_ = max_share_ == 0.0;
}

int64_t DiscardableEntries::now() const {
    if (clock_) return clock_();
    return (int64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void DiscardableEntries::used(int32_t id, uint32_t gen) {
    if (!enabled()) return;
    last_use_[id] = gen;
    uses_.emplace_back(id, gen);
}

void DiscardableEntries::offer_entry(int32_t id) {
    const int64_t t0 = timing_ ? now() : 0;
    bool any = false;
    size_t bytes = 0;
    spans_(id, [&](Span s) {
        if (ops_->offer(s)) {
            any = true;
            bytes += s.size;
        }
    });
    offered_[id] = any ? 1 : 0;
    offered_size_[id] = bytes;
    offered_total_ += bytes;
    if (timing_) window_cost_ns_ += now() - t0;
}

// The wall is the sum of priced spans, so the time of a prompt is out of it, and so is the pause
// before one. A pause that no prompt ends (a turn too short to be a wide batch) stays in, and only
// makes that window price cheap: it can delay the verdict but never invent one.
void DiscardableEntries::price_window() {
    if (pressure_only_ || max_share_ < 0 || window_loads_seen_ < loads_per_token_ ||
        window_round_trips_ < price_window_round_trips || window_wall_ns_ <= 0)
        return;
    const double share = (double) window_cost_ns_ / (double) window_wall_ns_;
    const bool full = window_loads_seen_ >= loads_per_token_ * price_window_tokens;
    if (share > max_share_ * price_fast_factor) { // plain at once, from one token on
        cost_share_ = share;
        pressure_only_ = true;
    } else if (full) {
        cost_share_ = share;
        if (share > max_share_) pressure_only_ = true;
    } else {
        return; // near the cap: let the window fill
    }
    window_loads_seen_ = 0;
    window_wall_ns_ = window_cost_ns_ = 0;
    window_round_trips_ = 0;
}

bool DiscardableEntries::offer_cold(uint32_t gen, const std::function<bool(int32_t)> & valid, bool priced) {
    if (!enabled()) return false;
    const bool was_pressure_only = pressure_only_;
    if (max_share_ >= 0 && !pressure_only_) {
        const int64_t t = now();
        if (timing_ && span_t0_ >= 0) window_wall_ns_ += t - span_t0_; // the span the last call opened
        price_window();
        timing_ = priced && !pressure_only_;
        span_t0_ = timing_ ? t : -1;
        if (timing_) ++window_loads_seen_;
    } else {
        timing_ = false;
    }
    const auto stale = [&](int32_t id, uint32_t g) { return offered_[id] || last_use_[id] != g || !valid(id); };
    while (!uses_.empty() && uses_.front().second + offer_after_ <= gen) {
        const auto [id, g] = uses_.front();
        uses_.pop_front();
        if (stale(id, g)) continue;
        if (pressure_only_)
            cold_.emplace_back(id, g);
        else
            offer_entry(id);
    }
    if (pressure_only_) {
        while (offered_total_ < shortfall_ && !cold_.empty()) {
            const auto [id, g] = cold_.front();
            cold_.pop_front();
            if (!stale(id, g)) offer_entry(id);
        }
        // An entry used again leaves its older pair here; drop those before they outnumber the cache.
        if (cold_.size() > 2 * offered_.size()) {
            std::deque<std::pair<int32_t, uint32_t>> live;
            for (const auto & [id, g] : cold_)
                if (!stale(id, g)) live.emplace_back(id, g);
            cold_.swap(live);
        }
    }
    return pressure_only_ && !was_pressure_only;
}

void DiscardableEntries::forget(int32_t id) {
    if (!offered(id)) return;
    offered_[id] = 0;
    offered_total_ -= offered_size_[id];
    offered_size_[id] = 0;
}

bool DiscardableEntries::take_back(int32_t id) {
    if (!offered(id)) return true;
    offered_[id] = 0;
    offered_total_ -= offered_size_[id];
    offered_size_[id] = 0;
    const int64_t t0 = timing_ ? now() : 0;
    bool intact = true;
    spans_(id, [&](Span s) {
        if (drop_before_reclaim_) ops_->drop(s);
        intact = ops_->reclaim(s) && intact; // every span, not short-circuited: all must come back
    });
    if (timing_) {
        window_cost_ns_ += now() - t0;
        if (intact) ++window_round_trips_; // a kept offer: the premium paid for nothing this time
    }
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
