#include "bmoe/cache_governor.h"

#include <algorithm>

namespace bmoe {

CacheGovernor::CacheGovernor(const CacheGovernorParams & p) : p_(p) {
    cap_ = std::min(p_.initial, p_.user_cap);
    cap_ = std::max(cap_, std::min(p_.min_cap, p_.user_cap));
    // The cooldown paces cuts APART; it must not delay the first one. Starting it spent lets the
    // very first token act — which is the token that matters most, because a session opened onto an
    // already-pressured device is in the war before it generates anything.
    since_cut_ = p_.cut_cooldown;
}

// The mechanical floor: the largest layer we must be able to stage, once the streamer has measured
// it, and the configured minimum until then. Deliberately NOT one token's working set — see the
// note in the header. Never above the cap the user allowed.
size_t CacheGovernor::floor_of(const CacheSignals & s) const {
    return std::min(s.floor ? s.floor : p_.min_cap, p_.user_cap);
}

// Grow freely up to the user's cap, but stay a margin below any budget already known to provoke
// reclaim — re-touching the ceiling just to be cut again is a war with extra steps.
size_t CacheGovernor::grow_limit() const {
    if (ceiling_ == no_ceiling) return p_.user_cap;
    return std::min(p_.user_cap, (size_t) ((double) ceiling_ * p_.ceiling_retreat));
}

CacheGovernor::Decision CacheGovernor::on_token(const CacheSignals & s) {
    if (since_cut_ < p_.cut_cooldown) ++since_cut_;

    // Residency first: it is absolute (our pages are either in RAM or they are not), so it needs
    // no baseline and cannot be fooled by faults that belong to someone else's memory. The fault
    // reflex only covers the tokens between samples, and only once a calm baseline exists to
    // compare against — without one, every value looks extreme.
    const bool sampled = s.resident_frac >= 0.0;
    bool pressure = false;
    if (sampled && s.resident_frac < p_.resident_min) {
        pressure = true;
    } else if (baseline_ >= 0.0 && (double) s.majflt > baseline_ * p_.fault_ratio + (double) p_.fault_margin) {
        pressure = true;
    }

    const size_t floor = floor_of(s);

    if (pressure) {
        calm_ = 0;
        // Keep cutting for as long as the device keeps taking memory, all the way down to the
        // mechanical floor. There is no budget worth defending against a kernel that will not
        // concede it: the hits a smaller cache gives up are bounded, the war is not.
        if (since_cut_ >= p_.cut_cooldown && cap_ > floor) {
            ceiling_ = cap_;
            cap_ = std::max((size_t) ((double) cap_ * p_.cut_factor), floor);
            since_cut_ = 0;
            ++cuts_;
            return {cap_, true};
        }
        return {cap_, false};
    }

    // Calm. Learn what this device's quiet fault rate looks like, so the reflex is calibrated to
    // it rather than to a number we guessed. Only calm tokens feed the baseline — sampling a storm
    // would teach the loop that the storm is normal.
    baseline_ = baseline_ < 0.0 ? (double) s.majflt
                                : p_.baseline_decay * baseline_ + (1.0 - p_.baseline_decay) * (double) s.majflt;

    if (++calm_ < p_.calm_tokens) return {cap_, false};

    const size_t limit = grow_limit();
    if (cap_ < limit) {
        cap_ = std::min(cap_ + p_.grow_step, limit);
        calm_ = 0;
        since_probe_ = 0;
        return {cap_, true};
    }

    // Pinned at a remembered ceiling and calm for a long time: the concession may have moved (a
    // background app exited, the user left the camera). Forget the ceiling and let ordinary growth
    // re-discover it. Being wrong costs exactly one cut, which is the price of not staying small
    // forever because of one bad minute.
    if (ceiling_ != no_ceiling && ++since_probe_ >= p_.probe_after) {
        since_probe_ = 0;
        ceiling_ = no_ceiling;
    }
    return {cap_, false};
}

} // namespace bmoe
