// Load-time calibration's loop (bmoe/calibrate.h): the planner names the side, a candidate times it,
// the record grows, and the planner is asked again.

#include "bmoe/calibrate.h"

#include "bmoe/log.h"
#include "bmoe/params.h"

#include <string>
#include <vector>

namespace bmoe {

namespace {

// Which decision a plan's trial names (0 the device tier, 1 the graph width, -1 none) and its side,
// in the order record_side_rate reads them: one decision per run.
int decision_of(const KnobTrialConfig & t) {
    return t.device_tier >= 0 ? 0 : t.width_side >= 0 ? 1 : -1;
}
int side_of(const KnobTrialConfig & t) {
    return t.device_tier >= 0 ? t.device_tier : t.width_side;
}

std::string side_words(const KnobTrialConfig & t, const RunConfig & c) {
    if (t.device_tier >= 0) return t.device_tier ? "with a device expert cache" : "with the fitter's placement";
    const int width = t.width_side ? (c.n_ubatch > 0 ? c.n_ubatch : c.n_ctx) : t.width_narrow;
    return std::string("with the graph ") + (t.width_side ? "at the plan's width" : "narrowed") + " (" +
           std::to_string(width) + " tokens wide)";
}

// A run, under the side it answered.
struct Timed {
    std::vector<std::string> identity; // the candidate's config, as its arguments
    KnobTrialConfig trial;
    CandidateTiming timing;
};

} // namespace

bool side_pending(const Plan & p) {
    return p.config.trial.device_tier >= 0 || p.config.trial.width_side >= 0;
}

RunConfig calibration_candidate(const RunConfig & planned) {
    RunConfig c = planned;
    KnobTrialConfig t;
    t.window_tokens = planned.trial.window_tokens; // the windows the candidate is timed in
    c.trial = t;
    return c;
}

CalibrationResult calibrate(const Plan & first,
                            const MeasuredKnobs & start,
                            const Replan & replan,
                            const RunCandidate & run,
                            const CalibrationPolicy & pol) {
    CalibrationResult r;
    r.plan = first;
    r.knobs = start;
    std::vector<Timed> timed;
    std::vector<int32_t> reference; // the first run's tokens, which every later run replays
    // A run's rate answers its own side, and at most one side of the other decision when a later plan
    // asks for the same configuration again: past that, the plan is asking in a circle.
    const int steps_max = k_calibration_decisions * pol.runs_max;
    for (int step = 0; side_pending(r.plan); ++step) {
        if (step >= steps_max || !run || !replan) break;
        const KnobTrialConfig side = r.plan.config.trial;
        const RunConfig cand = calibration_candidate(r.plan.config);
        const std::vector<std::string> identity = to_args(cand);
        const std::string words = side_words(side, cand);

        // A configuration already timed is not opened again: two plans that ask for the same load ask
        // the same question, whichever decision they ask it for.
        const Timed * same = nullptr;
        for (const Timed & t : timed)
            if (t.identity == identity && t.timing.created) same = &t;
        CandidateTiming timing;
        if (same) {
            timing = same->timing;
        } else {
            if (r.runs >= pol.runs_max) {
                log_printf(LogLevel::Info,
                           "calibration: stopped after %d candidate loads with a side still pending; the served "
                           "load times it",
                           r.runs);
                break;
            }
            CalibrationRun cr;
            cr.index = r.runs;
            cr.runs_max = pol.runs_max;
            cr.replay = reference;
            // The other side of this decision, where it ran: past its range by the margin, this one has lost.
            for (const Timed & t : timed)
                if (t.timing.created && decision_of(t.trial) == decision_of(side) &&
                    side_of(t.trial) == 1 - side_of(side) &&
                    (decision_of(side) == 0 || t.trial.width_narrow == side.width_narrow)) {
                    cr.reference_lo = t.timing.step_lo;
                    cr.reference_hi = t.timing.step_hi;
                }
            log_printf(LogLevel::Info, "calibration: timing the decode %s (candidate %d of at most %d)", words.c_str(),
                       cr.index + 1, cr.runs_max);
            timing = run(cand, cr);
            ++r.runs;
            if (timing.aborted) {
                r.aborted = true;
                log_printf(LogLevel::Info, "calibration: stopped before its verdict");
                break;
            }
        }

        const std::string before = format_measured_knobs({r.knobs});
        if (!timing.created) {
            record_side_unviable(side, r.knobs);
            log_printf(LogLevel::Info, "calibration: the candidate %s could not be opened (%s); that side loses",
                       words.c_str(), timing.error.c_str());
        } else if (timing.tokens_per_second > 0.0) {
            record_side_rate(side, timing.tokens_per_second, r.knobs);
            if (!same)
                log_printf(LogLevel::Info, "calibration: %.2f tok/s %s, %s", timing.tokens_per_second, words.c_str(),
                           timing.cut ? "cut once past the other side's range"
                                      : ("the median of " + std::to_string(timing.windows) + " windows").c_str());
            if (reference.empty()) reference = timing.tokens;
        }
        timed.push_back({identity, side, timing});
        // Nothing new in the record: the plan would ask the same again.
        if (format_measured_knobs({r.knobs}) == before) {
            log_printf(LogLevel::Info,
                       "calibration: the decode %s gave the plan nothing new; the served load times "
                       "what is left",
                       words.c_str());
            break;
        }
        r.plan = replan(r.knobs);
    }
    r.complete = !side_pending(r.plan);
    return r;
}

} // namespace bmoe
