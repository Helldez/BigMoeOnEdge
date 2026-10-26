#include "knob_trial.h"

#include <algorithm>
#include <cmath>

namespace bmoe {

KnobTrial::KnobTrial(int n_candidates, bool settle, KnobTrialPolicy pol) : pol_(pol), n_cand_(n_candidates) {
    pol_.window_tokens = std::max(1, pol_.window_tokens);
    pol_.warm_tokens = std::max(0, pol_.warm_tokens);
    pol_.rounds = std::max(1, pol_.rounds);
    // Every candidate window is bracketed by incumbent ones, so the schedule starts and ends on the
    // incumbent and alternates in between: 0 c1 0 c2 0 c1 0 c2 0 for two candidates over two rounds.
    schedule_.push_back(0);
    for (int r = 0; r < pol_.rounds; ++r)
        for (int c = 1; c <= n_cand_; ++c) {
            schedule_.push_back(c);
            schedule_.push_back(0);
        }
    if (n_cand_ <= 0)
        phase_ = Phase::Done;
    else if (!settle)
        phase_ = Phase::Measure;
    // The first step of a trial follows whatever ran before it, a prefill or another knob's switch.
    warm_left_ = pol_.warm_tokens;
}

int KnobTrial::arm() const {
    if (phase_ != Phase::Measure) return phase_ == Phase::Done ? winner_ : 0;
    return schedule_[spt_.size()];
}

void KnobTrial::interrupt() {
    tokens_ = 0;
    seconds_ = 0.0;
    lookups_ = hits_ = 0;
    warm_left_ = pol_.warm_tokens;
}

void KnobTrial::record(double seconds, int tokens, uint64_t lookups, uint64_t hits) {
    if (phase_ == Phase::Done || tokens <= 0 || !(seconds > 0.0)) return;
    if (warm_left_ > 0) {
        warm_left_ -= tokens; // a step is timed whole or not at all
        return;
    }
    tokens_ += tokens;
    seconds_ += seconds;
    lookups_ += lookups;
    hits_ += hits;
    if (tokens_ >= pol_.window_tokens) close_window();
}

void KnobTrial::close_window() {
    const double spt = seconds_ / (double) tokens_;
    const double hit = lookups_ ? (double) hits_ / (double) lookups_ : -1.0;
    const int prev_arm = arm();
    tokens_ = 0;
    seconds_ = 0.0;
    lookups_ = hits_ = 0;

    if (phase_ == Phase::Settle) {
        ++settle_windows_;
        // Without a cache there is nothing to settle; otherwise two windows in a row that hit alike.
        const bool settled = hit < 0.0 || (last_hit_ >= 0.0 && std::fabs(hit - last_hit_) <= pol_.settle_hit_delta) ||
                             settle_windows_ >= pol_.settle_windows_max;
        last_hit_ = hit;
        if (settled) phase_ = Phase::Measure; // still the incumbent: no switch, no warm-up
        return;
    }

    spt_.push_back(spt);
    if (spt_.size() == schedule_.size()) {
        decide();
        return;
    }
    if (arm() != prev_arm) warm_left_ = pol_.warm_tokens;
}

std::vector<double> KnobTrial::ratios(int cand) const {
    std::vector<double> r;
    // A candidate window counts once the incumbent window after it has closed too.
    for (size_t k = 1; k + 1 < spt_.size(); ++k) {
        if (schedule_[k] != cand) continue;
        const double around = 0.5 * (spt_[k - 1] + spt_[k + 1]);
        if (around > 0.0) r.push_back(spt_[k] / around);
    }
    return r;
}

static double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double KnobTrial::gain(int cand) const {
    const double m = median(ratios(cand));
    return m > 0.0 ? 1.0 / m : 0.0;
}

int KnobTrial::rounds(int cand) const {
    return (int) ratios(cand).size();
}

void KnobTrial::decide() {
    phase_ = Phase::Done;
    winner_ = 0;
    double best = 1.0 + pol_.min_win;
    for (int c = 1; c <= n_cand_; ++c) {
        const double g = gain(c);
        if (g >= best) {
            best = g;
            winner_ = c;
        }
    }
}

} // namespace bmoe
