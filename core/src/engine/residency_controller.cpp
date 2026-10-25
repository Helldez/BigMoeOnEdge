#include "residency_controller.h"

#include <algorithm>

namespace bmoe {

void ResidencyController::start(size_t planned, size_t floor) {
    floor_ = floor;
    ceiling_ = std::max(planned, floor);
}

double ResidencyController::reference() const {
    if (seen_ < pol_.warmup_tokens || recent_.empty()) return 0.0;
    std::vector<double> v = recent_;
    std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)(v.size() / 2), v.end());
    return v[v.size() / 2];
}

bool ResidencyController::stalled(double compute_s) {
    ++seen_;
    const double ref = reference();
    const bool stall = ref > 0.0 && compute_s >= pol_.stall_factor * ref;
    // Warm-up tokens are not kept: while the cache fills, compute is not what it will settle to.
    if (!stall && seen_ > pol_.warmup_tokens / 2) {
        recent_.push_back(compute_s);
        if ((int) recent_.size() > pol_.window) recent_.erase(recent_.begin());
    }
    return stall;
}

size_t ResidencyController::braked(size_t budget, size_t floor) {
    if (budget <= floor) return budget;
    return std::max(budget / 2, floor);
}

size_t ResidencyController::on_token(double compute_s,
                                     const OwnMemory & own,
                                     size_t budget,
                                     const std::function<double()> & pages_lost) {
    event_ = ResidencyEvent::none;
    lost_share_ = 0.0;
    lost_delta_ = 0;
    const bool stall = stalled(compute_s);
    if (floor_ == 0 || budget == 0) return budget; // no cache to size
    const uint64_t margin = std::max<uint64_t>(1, budget / (size_t) std::max(1, pol_.loss_margin_div));

    if (!stall) {
        if (++clean_streak_ == 1) streak_lost_ = own.lost_bytes;
        if (clean_streak_ >= pol_.window) in_episode_ = false;
        if (own.known) {
            clean_known_ = true;
            clean_lost_ = own.lost_bytes;
        }
        if (recover_every_ == 0 || budget >= ceiling_ || clean_streak_ < recover_every_) return budget;
        clean_streak_ = 0;
        // Calm is not enough where the system is still taking memory without stalling us yet:
        // climbing then is how a reclaim becomes a war.
        if (own.known && own.lost_bytes > streak_lost_ + margin) return budget;
        event_ = ResidencyEvent::recover;
        return std::min(budget + floor_, ceiling_);
    }

    // A stall right after a brake belongs to the same episode: the half given back was not enough.
    const bool same_episode = in_episode_ && clean_streak_ < pol_.window;
    clean_streak_ = 0;
    bool confirmed = false;
    if (own.known && clean_known_) {
        // Against what was lost while things were last calm. A brake does not reset it: as long as
        // the stalls go on and the memory has not come back, each one is a reason to give more.
        if (own.lost_bytes > clean_lost_ + margin) {
            confirmed = true;
            lost_delta_ = own.lost_bytes - clean_lost_;
        }
    }
    if (!confirmed) {
        const double share = pages_lost ? pages_lost() : -1.0;
        if (share > 0.0) {
            confirmed = true;
            lost_share_ = share;
        }
    }
    if (!confirmed) return budget; // slow, but not because memory went: the cores, the clock, a neighbour

    const size_t next = braked(budget, floor_);
    if (next >= budget) return budget;
    event_ = ResidencyEvent::brake;
    if (!same_episode) {
        // The size this episode started from is the one the machine refused; the halvings within
        // it are the search for what it can keep, not further refusals, so they do not lower the
        // ceiling again. Each new episode makes the next recovery wait twice as long.
        ceiling_ = std::max(floor_, std::min(ceiling_, budget - floor_));
        recover_every_ = recover_every_ == 0 ? pol_.recover_after : std::min(recover_every_ * 2, 1 << 20);
    }
    in_episode_ = true;
    return next;
}

} // namespace bmoe
