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
    const int width = t.width_side ? c.graph_width() : t.width_narrow;
    return std::string("with the graph ") + (t.width_side ? "at the plan's width" : "narrowed") + " (" +
           std::to_string(width) + " tokens wide)";
}

// What the plan reads of a record: all of it, failed opens included.
std::string record_state(const MeasuredKnobs & k) {
    return format_measured_knobs({k});
}

// Every side a record counts failed opens of, as the trial that names it.
std::vector<KnobTrialConfig> counted_sides(const MeasuredKnobs & k) {
    std::vector<KnobTrialConfig> v;
    for (int s : {1, 0}) {
        KnobTrialConfig t;
        t.device_tier = s;
        v.push_back(t);
    }
    for (int s : {1, 0}) {
        KnobTrialConfig t;
        t.width_side = s;
        t.width_narrow = k.width_narrow;
        v.push_back(t);
    }
    return v;
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

bool calibration_due(const Plan & p,
                     const MeasuredKnobs & knobs,
                     const Replan & replan,
                     const CalibrationPolicy & pol) {
    if (side_pending(p)) return true;
    // A side due to be tried again is due only if the plan would ask for it with its count set aside:
    // a decision a person pinned, or a narrow width that has moved, asks for nothing.
    MeasuredKnobs k = knobs;
    bool offered = false;
    for (const KnobTrialConfig & t : counted_sides(k)) {
        int * n = unviable_count(t, k);
        if (*n > 0 && *n < pol.unviable_retries) {
            *n = 0;
            offered = true;
        }
    }
    return offered && replan && side_pending(replan(k));
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
    // A side that failed to open on an earlier load is offered again while its count is under the
    // retry budget: the record the plan reads leaves it out, and the plan asks for it if it would.
    MeasuredKnobs kept = start;
    std::vector<KnobTrialConfig> offered;
    for (const KnobTrialConfig & t : counted_sides(start)) {
        int * n = unviable_count(t, r.knobs);
        if (*n > 0 && *n < pol.unviable_retries) {
            *n = 0;
            offered.push_back(t);
        }
    }
    if (!offered.empty() && replan) r.plan = replan(r.knobs);
    std::vector<KnobTrialConfig> tried; // sides opened (or found unopenable) by this calibration
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

        const std::string before = record_state(r.knobs);
        tried.push_back(side);
        if (!timing.created) {
            // One more failure on top of what the record kept for this side.
            KnobTrialConfig at = side;
            const bool same_width = decision_of(side) == 0 || kept.width_narrow == side.width_narrow;
            const int before_n = same_width ? *unviable_count(at, kept) : 0;
            record_side_unviable(side, r.knobs);
            *unviable_count(side, r.knobs) = before_n + 1;
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
        if (record_state(r.knobs) == before) {
            log_printf(LogLevel::Info,
                       "calibration: the decode %s gave the plan nothing new; the served load times "
                       "what is left",
                       words.c_str());
            break;
        }
        r.plan = replan(r.knobs);
    }
    // A side offered again but not reached (stopped, or the plan never asked for it) keeps its count.
    bool restored = false;
    for (const KnobTrialConfig & t : offered) {
        bool reached = false;
        for (const KnobTrialConfig & u : tried)
            reached = reached || (decision_of(u) == decision_of(t) && side_of(u) == side_of(t));
        if (reached || (decision_of(t) == 1 && r.knobs.width_narrow != kept.width_narrow)) continue;
        KnobTrialConfig at = t;
        int * n = unviable_count(at, r.knobs);
        if (*n == 0) {
            *n = *unviable_count(at, kept);
            restored = true;
        }
    }
    if (restored && replan) r.plan = replan(r.knobs);
    r.complete = !side_pending(r.plan);
    return r;
}

} // namespace bmoe
