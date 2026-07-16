#include "bmoe/cache_governor.h"

#include <algorithm>
#include <cmath>

namespace bmoe {

namespace {
// The upper bound configuration puts on the budget: an explicit cap, else the physical expert set,
// else unbounded. Used wherever v1 wrote p_.user_cap directly, so a 0 user_cap means "no config cap".
size_t config_hi(const CacheGovernorParams & p) {
    if (p.user_cap) return p.user_cap;
    if (p.phys_cap) return p.phys_cap;
    return CacheGovernor::no_ceiling;
}
} // namespace

CacheGovernor::CacheGovernor(const CacheGovernorParams & p) : p_(p) {
    const size_t hi = config_hi(p_);
    cap_ = std::min(p_.initial, hi);
    cap_ = std::max(cap_, std::min(p_.min_cap, hi));
    // The cooldown paces cuts APART; it must not delay the first one. Starting it spent lets the
    // very first token act — which is the token that matters most, because a session opened onto an
    // already-pressured device is in the war before it generates anything.
    since_cut_ = p_.cut_cooldown;
}

// The mechanical floor: the largest layer we must be able to stage, once the streamer has measured
// it, and the configured minimum until then. Deliberately NOT one token's working set — see the
// note in the header. Never above the cap configuration allowed.
size_t CacheGovernor::floor_of(const CacheSignals & s) const {
    return std::min(s.floor ? s.floor : p_.min_cap, config_hi(p_));
}

// Grow freely up to the configured cap, but stay below any budget already known to provoke reclaim,
// the physical set size, and the hit-rate plateau. Re-touching a bound just to be cut again is a war
// with extra steps.
size_t CacheGovernor::grow_limit(const CacheSignals & /*s*/) const {
    size_t lim = config_hi(p_);
    if (ceiling_ != no_ceiling) lim = std::min(lim, (size_t) ((double) ceiling_ * p_.ceiling_retreat));
    if (plateau_cap_ != no_ceiling) lim = std::min(lim, plateau_cap_);
    return lim;
}

// The device will not concede a cache big enough to hold one token's demand, so no budget it allows
// can earn hits — the cache is pure churn. A rare clean fast-path to demotion; the workhorse is the
// unrelieved-war test. Needs a decode-measured demand and a device reading (both 0 in unit tests
// with default signals, so this stays inert there).
bool CacheGovernor::infeasible(const CacheSignals & s) const {
    if (s.token_demand == 0 || s.mem_available == 0) return false;
    const size_t room = s.mem_available > p_.floor_headroom ? (size_t) (s.mem_available - p_.floor_headroom) : 0;
    const size_t sustainable = cap_ + room; // the largest cache the device would concede right now
    return s.token_demand > sustainable;
}

CacheGovernor::War CacheGovernor::classify(const CacheSignals & s, bool & swap_rising, bool & dense_declining) {
    // Swap EWMA: only positive deltas (fresh compression of our anon memory) feed it; a calm token's
    // zero or negative delta decays it back down.
    const double inc = s.swap_delta > 0 ? (double) s.swap_delta : 0.0;
    swap_ewma_ = 0.5 * swap_ewma_ + 0.5 * inc;
    // A single large jump is a war on the spot (the kernel swaps a chunk at once); the EWMA catches
    // a slower, sustained climb the raw delta alone would miss.
    swap_rising = s.swap_delta > (int64_t) p_.swap_margin || swap_ewma_ > (double) p_.swap_margin;

    // Dense plateau: the running high-watermark of what the model's working set normally holds, so a
    // decline is judged relative to THIS model on THIS device, not an absolute the sensor never hits.
    if (s.dense_resident_frac >= 0.0) {
        if (dense_plateau_ < 0.0 || s.dense_resident_frac > dense_plateau_) dense_plateau_ = s.dense_resident_frac;
        ++dense_samples_;
    }
    dense_declining = s.dense_resident_frac >= 0.0 && dense_plateau_ >= 0.0 && dense_samples_ >= 4 &&
                      s.dense_resident_frac < dense_plateau_ * p_.dense_decline;

    const bool resident_sampled = s.resident_frac >= 0.0;
    const bool cache_reclaim = resident_sampled && s.resident_frac < p_.resident_min;
    const bool cache_fine = resident_sampled && s.resident_frac >= p_.resident_min;
    const bool fault_reflex =
        baseline_ >= 0.0 && (double) s.majflt > baseline_ * p_.fault_ratio + (double) p_.fault_margin;

    // Order is deliberate: a directly observed cache reclaim first (no baseline needed), then the
    // dense war (an early warning that fires on the residency dip, before the faults pile up), then
    // the anon war (swap climbing under a fault load), then the reflex as a proxy for cache reclaim
    // when we could not sample it, and finally faults we can prove are not ours.
    if (cache_reclaim) return War::cache;
    if (dense_declining) return War::dense;
    if (swap_rising && fault_reflex) return War::anon;
    if (fault_reflex && !cache_fine) return War::cache; // faults, cache not provably resident → assume ours
    if (fault_reflex) return War::foreign;              // faults, cache demonstrably resident → someone else
    return War::none;
}

// Measure the hit rate over the interval just ended (at the current cap) against the previous one.
// Called just before a grow: if the extra budget of the last step stopped buying hits, freeze here.
void CacheGovernor::plateau_on_grow(const CacheSignals & s) {
    if (grow_sampled_) {
        const long long dl = s.cache_lookups - grow_lookups_;
        if (dl > 0) {
            const double h = (double) (s.cache_hits - grow_hits_) / (double) dl;
            if (prev_hitrate_ >= 0.0) {
                const double noise = 2.0 * std::sqrt(std::max(0.0, h * (1.0 - h)) / (double) dl);
                if (h - prev_hitrate_ <= noise) {
                    if (++plateau_miss_ >= p_.plateau_confirm) plateau_cap_ = cap_; // more RAM stopped earning
                } else {
                    plateau_miss_ = 0;
                }
            }
            prev_hitrate_ = h;
        }
    }
    grow_hits_ = s.cache_hits;
    grow_lookups_ = s.cache_lookups;
    grow_sampled_ = true;
}

CacheGovernor::Decision CacheGovernor::demote() {
    state_ = State::off;
    rearm_ok_ = 0;
    since_rearm_probe_ = 0;
    // No dense re-warm here: measured, the memory war has hysteresis. Once the war has pushed the
    // dense weights off the kernel's active list, the slot-mode read churn reclaims them again before
    // a re-warm's pages can be re-referenced, so restoring them cannot win (the refuted PR #28). The
    // only cure is prevention — not entering the war — which is why demoting mid-war on a >RAM
    // no-reuse model recovers poorly and cache-off from cold is the real ceiling for such a model.
    // cap_ is kept as the last LRU budget so re-arm has a level to return toward.
    return {cap_, false, Mode::slots, false};
}

CacheGovernor::Decision CacheGovernor::off_tick(const CacheSignals & s) {
    war_ = War::none;
    if (++since_rearm_probe_ < p_.probe_every) return {cap_, false, Mode::keep, false};
    since_rearm_probe_ = 0;
    bool ok = false;
    if (s.mem_available > 0 && s.token_demand > 0) {
        const size_t room = s.mem_available > p_.floor_headroom ? (size_t) (s.mem_available - p_.floor_headroom) : 0;
        ok = (double) room > (double) s.token_demand * p_.rearm_margin;
    }
    if (ok) {
        if (++rearm_ok_ >= p_.rearm_persist) return promote(s);
    } else {
        rearm_ok_ = 0;
    }
    return {cap_, false, Mode::keep, false};
}

CacheGovernor::Decision CacheGovernor::promote(const CacheSignals & s) {
    state_ = State::on;
    war_ = War::none;
    ceiling_ = no_ceiling;
    plateau_cap_ = no_ceiling;
    calm_ = 0;
    since_cut_ = p_.cut_cooldown;
    war_cuts_ = 0;
    rewarmed_ = false;
    grow_sampled_ = false;
    prev_hitrate_ = -1.0;
    plateau_miss_ = 0;
    below_demand_ = 0;
    hr_win_ = 0;
    hr_grace_ = 2;
    fault_ewma_ = 0.0;
    // Restart the LRU at a demand-sized budget within the room we just confirmed, then climb by
    // utility. Never below the mechanical floor.
    const size_t room = s.mem_available > p_.floor_headroom ? (size_t) (s.mem_available - p_.floor_headroom) : 0;
    cap_ = std::max(floor_of(s), std::min(cap_, room ? room : cap_));
    return {cap_, true, Mode::lru, false};
}

CacheGovernor::Decision CacheGovernor::on_token(const CacheSignals & s) {
    if (since_cut_ < p_.cut_cooldown) ++since_cut_;

    if (state_ == State::off) return off_tick(s);

    bool swap_rising = false, dense_declining = false;
    war_ = classify(s, swap_rising, dense_declining);
    const size_t floor = floor_of(s);

    // Feasibility fast-path: if the device will not concede a cache large enough to hold one token's
    // demand, no allowed budget can earn hits and the churn is pure cost — go straight to slots
    // rather than fight a war for a cache that cannot work. Inert until a decode demand is known.
    if (infeasible(s)) return demote();

    // Utility demote — the direct, measured read of "the cache is not earning", which the fault
    // reflex (baseline can't calibrate in a never-calm war) and the MemAvailable feasibility test can
    // both miss. Two independent triggers, either sufficient:
    //   (a) a cut has driven the budget below one token's demand: it holds nothing between tokens; or
    //   (b) a window of near-zero hit rate: the model has no reuse the cache can exploit.
    // Either way the LRU is pure commit/evict churn, and slot mode (no churn) is strictly better.
    fault_ewma_ = p_.fault_ewma_decay * fault_ewma_ + (1.0 - p_.fault_ewma_decay) * (double) s.majflt;
    if (s.token_demand > 0) {
        if (ceiling_ != no_ceiling && cap_ < s.token_demand) {
            if (++below_demand_ >= p_.demote_after) return demote();
        } else {
            below_demand_ = 0;
        }
        if (++hr_win_ >= p_.demote_window && s.cache_lookups > hr_lookups0_) {
            const long long dl = s.cache_lookups - hr_lookups0_;
            const double hr = (double) (s.cache_hits - hr_hits0_) / (double) dl;
            hr_hits0_ = s.cache_hits;
            hr_lookups0_ = s.cache_lookups;
            hr_win_ = 0;
            if (hr_grace_ > 0)
                --hr_grace_; // skip the warm-up windows: the cache is still filling, hits are low anyway
            else if (hr < p_.demote_hit_floor || fault_ewma_ > (double) p_.war_fault_abs)
                return demote(); // earns almost nothing, or a sustained fault war cutting cannot end
        }
    }

    const bool cutting_war = war_ == War::cache || war_ == War::dense || war_ == War::anon;

    if (cutting_war) {
        calm_ = 0;
        grow_sampled_ = false; // the plateau baseline is stale once a war moves the cap
        Decision d{cap_, false, Mode::keep, false};

        if (since_cut_ >= p_.cut_cooldown && cap_ > floor) {
            ceiling_ = cap_;
            const double f = war_ == War::anon ? p_.cut_factor_hard : p_.cut_factor;
            cap_ = std::max((size_t) ((double) cap_ * f), floor);
            since_cut_ = 0;
            ++cuts_;
            ++war_cuts_;
            d = {cap_, true, Mode::keep, false};
            if (war_ == War::dense && !rewarmed_) {
                d.rewarm_dense = true;
                rewarmed_ = true;
            }
        }

        // Demote when cutting cannot end the war. The anon/zram war is churn the LRU itself feeds, so
        // cutting only slows the bleed; measured, it survived repeated cuts to the floor while slot
        // mode ended it. Also demote on the clean feasibility fast-path.
        const bool churn_war = war_ == War::anon;
        const bool at_floor = cap_ <= floor;
        if ((churn_war && war_cuts_ >= p_.demote_after) || (churn_war && at_floor && war_cuts_ >= 1)) return demote();
        return d;
    }

    // ── not a war we cut for ──
    war_cuts_ = 0;
    rewarmed_ = false;

    if (war_ == War::foreign) {
        // Faults, but our cache and the dense weights are demonstrably resident — the load is the
        // device's, not ours. Absorb it into the baseline so we stop re-flagging it, but do not grow
        // this token: someone else is under pressure and growing would join their war.
        baseline_ = baseline_ < 0.0 ? (double) s.majflt
                                    : p_.baseline_decay * baseline_ + (1.0 - p_.baseline_decay) * (double) s.majflt;
        calm_ = 0;
        return {cap_, false, Mode::keep, false};
    }

    // Calm. Learn the quiet fault rate, but never from a token whose swap or dense already say a war
    // is brewing even if the fault reflex has not caught up — that is the baseline-poisoning fix.
    if (!swap_rising && !dense_declining)
        baseline_ = baseline_ < 0.0 ? (double) s.majflt
                                    : p_.baseline_decay * baseline_ + (1.0 - p_.baseline_decay) * (double) s.majflt;

    if (++calm_ < p_.calm_tokens) return {cap_, false, Mode::keep, false};

    size_t limit = grow_limit(s);
    const bool room = s.mem_available == 0 || s.mem_available > p_.floor_headroom + p_.grow_step;
    if (cap_ < limit && room) {
        plateau_on_grow(s); // may lower plateau_cap_ and thus the limit
        limit = grow_limit(s);
        if (cap_ < limit) {
            cap_ = std::min(cap_ + p_.grow_step, limit);
            calm_ = 0;
            since_probe_ = 0;
            return {cap_, true, Mode::keep, false};
        }
    }

    // Pinned (at a ceiling, a plateau, the physical size, or out of headroom) and calm for a long
    // time: the concession may have moved. Forget the learned bounds and let growth re-test them.
    if ((ceiling_ != no_ceiling || plateau_cap_ != no_ceiling) && ++since_probe_ >= p_.probe_after) {
        since_probe_ = 0;
        ceiling_ = no_ceiling;
        plateau_cap_ = no_ceiling;
    }
    return {cap_, false, Mode::keep, false};
}

} // namespace bmoe
