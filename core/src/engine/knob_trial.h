#pragma once

// The A/B of one lossless knob, run on the first real request (docs/measured-planning.md, Step 2).
//
// A knob that changes no output and can change with the session open - the decode thread count,
// whether reads overlap compute - is not a question for a rule: the run can answer it by trying, on
// the user's own answer, while it is decoded. What makes the answer worth keeping is that the
// comparison is matched. A day of A/B on a phone was once invalidated by throttling, the baseline
// itself falling from 6.54 to 3.93 tok/s as the device heated (docs/expert-prediction.md). So the
// windows are interleaved, every candidate window sits between two windows of the incumbent, and a
// candidate is compared with the mean of the two around it: a drift that is linear over the three
// windows cancels exactly, and what is left of a slower one is shared by both arms of the round.
// The verdict is the median of those per-round ratios, so one window that met a stall elsewhere in
// the machine does not decide it, and a candidate wins only past the margin the device rule
// already uses: a gain inside the measurement's own repeatability is not one.
//
// The trial starts once the cache's hit rate has settled. The first tokens of a session fill an
// empty cache, and a window then is a window of cold reads, whatever knob it ran under.
//
// Pure policy: the session feeds it one decode step at a time - its wall seconds, the tokens it
// produced, the cache lookups and hits it made - and asks which arm the next step runs under. It
// switches nothing itself.

#include "bmoe/planner.h"

#include <cstdint>
#include <vector>

namespace bmoe {

// Not measured constants: a window long enough to hold a few tokens of a decode, few enough
// windows that the trial ends inside the first answer, and a settling test that only asks whether
// two windows in a row hit the cache alike.
struct KnobTrialPolicy {
    int window_tokens = 8;          // tokens a timed window holds (RunConfig::trial.window_tokens)
    int warm_tokens = 1;            // tokens after a switch that are not timed: the switch's own cost
    int rounds = 3;                 // timed windows per candidate, each between two incumbent windows
    double settle_hit_delta = 0.02; // two incumbent windows this close in hit rate: the cache has settled
    int settle_windows_max = 8;     // a cache that never settles is measured anyway after this many
    // How much faster a candidate must be than the incumbent before it replaces it, as a fraction:
    // the planner's own margin for "a device is faster than the host", for the same reason.
    double min_win = PlannerPolicy::defaults().min_backend_win;
};

class KnobTrial {
public:
    // Arm 0 is the incumbent; arms 1..n_candidates the values tried against it. `settle`: wait for
    // the cache's hit rate to settle first (the first knob of a session; a later one starts at once).
    KnobTrial(int n_candidates, bool settle, KnobTrialPolicy pol = {});

    // The arm the next decode step must run under.
    int arm() const;
    bool done() const { return phase_ == Phase::Done; }
    bool settling() const { return phase_ == Phase::Settle; }

    // One decode step, run under arm(): its wall seconds, the tokens it produced, and the cache
    // lookups and hits it made (0 and 0 without a cache, which counts as settled).
    void record(double seconds, int tokens, uint64_t lookups, uint64_t hits);

    // The step stream was broken (the request ended): the window in progress is dropped, and the
    // next step is a warm-up again, since it follows a prefill rather than a token.
    void interrupt();

    // Once done(): the arm that won, 0 when the incumbent held.
    int winner() const { return winner_; }
    // How much faster arm `cand` (1..n) decoded than the incumbent around it, as a speed ratio (1.10
    // is 10% faster); 0 while it has no complete round.
    double gain(int cand) const;
    // Complete rounds measured for arm `cand`.
    int rounds(int cand) const;

    // The windows in the order they are run, and their seconds per token so far: for a log line.
    const std::vector<int> & schedule() const { return schedule_; }
    const std::vector<double> & window_seconds_per_token() const { return spt_; }

private:
    enum class Phase { Settle, Measure, Done };
    void close_window();
    void decide();
    std::vector<double> ratios(int cand) const;

    KnobTrialPolicy pol_;
    int n_cand_ = 0;
    Phase phase_ = Phase::Settle;
    std::vector<int> schedule_; // the arm of each timed window: 0 c1 0 c2 0 c1 0 ... 0
    std::vector<double> spt_;   // seconds per token of each window closed, in schedule order
    int warm_left_ = 0;
    int tokens_ = 0;
    double seconds_ = 0.0;
    uint64_t lookups_ = 0, hits_ = 0;
    double last_hit_ = -1.0;
    int settle_windows_ = 0;
    int winner_ = 0;
};

} // namespace bmoe
