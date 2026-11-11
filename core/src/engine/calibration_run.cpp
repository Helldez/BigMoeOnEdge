// A load-time calibration's candidate, run by the engine (bmoe/session.h, run_calibration_candidate):
// a session opened for the candidate's config, one short timed answer, closed.

#include "bmoe/session.h"

#include "../io/platform_io.h"
#include "../moe/gguf_offsets.h"
#include "knob_trial.h"

#include <atomic>
#include <memory>
#include <string>

namespace bmoe {

namespace {

// The one instruction every candidate answers. Generic, and asking for long ordinary prose: the
// answer must outlast the timing (a settle and its windows, a hundred-odd tokens at the default
// window) without ending early, and it should route the way a person's chat answer does, so no code,
// no list of short items, no tool call and nothing a model refuses. Rendered through the model's own
// chat template, as a served request is.
constexpr const char * k_calibration_prompt =
    "Write a long, detailed essay on how a city plans, builds and runs its public transport: buses, trams "
    "and trains, timetables, fares and funding, maintenance, and how it changes as the city grows. Use full "
    "paragraphs.";

// The first candidate samples rather than taking the argmax: greedy prose falls into loops, and a loop
// replays the same few experts, which a cache hits far more often than any real answer would let it.
// The seed is fixed so a calibration run again (one stopped by a request, picked up later) samples the
// same answer; a person's own sampling temperature is used where they set one.
constexpr float k_calibration_temp = 0.7f;
constexpr uint32_t k_calibration_seed = 1234;

// Every candidate starts from the same cold system cache. Whatever the previous candidate read
// through the model's mapping (the weights of a load, the dense part a streamed run reads) stays in
// the system's cache after it closes, and the next would load and decode from memory what the first
// read from the drive: the later side measured on a warmer machine, by however much the cache held.
// Handed back before each open, every shard of the model and the files beside it. Where the system
// offers no such advice (pio::drop_file_cache: Darwin, Windows) the bias stays, and favours the later
// side by what the cache still holds.
void drop_model_file_cache(const RunConfig & cfg) {
    const GgufOffsets offs = read_gguf_offsets(cfg.model_path.c_str());
    if (offs.ok)
        for (const std::string & p : offs.shard_paths)
            pio::drop_file_cache(p.c_str());
    else
        pio::drop_file_cache(cfg.model_path.c_str());
    for (const std::string * p : {&cfg.mmproj, &cfg.spec.mtp_head})
        if (!p->empty()) pio::drop_file_cache(p->c_str());
}

} // namespace

CandidateTiming
run_calibration_candidate(const RunConfig & cfg, const CalibrationRun & run, const std::atomic<bool> * stop) {
    CandidateTiming t;
    if (stop && stop->load(std::memory_order_relaxed)) {
        t.aborted = true;
        return t;
    }
    SessionConfig sc = session_config_from(cfg);
    sc.spec = SpecConfig{};
    // The candidate times itself (CalibrationTurn): no knob trial, no side named. The windows are the
    // trial's, which a test narrows.
    sc.trial = KnobTrialConfig{};
    sc.trial.window_tokens = cfg.trial.window_tokens;
    sc.chatml = true;
    if (!(sc.sampling.temp > 0.0f)) sc.sampling.temp = k_calibration_temp;
    sc.sampling.seed = k_calibration_seed;

    drop_model_file_cache(cfg);
    std::string error;
    std::unique_ptr<Session> s = Session::open(sc, error);
    // A load takes seconds to minutes and cannot be stopped inside: a stop raised meanwhile is honoured
    // as soon as it returns, before anything is decoded.
    if (stop && stop->load(std::memory_order_relaxed)) {
        t.aborted = true;
        return t;
    }
    if (!s) {
        t.error = error.empty() ? "the candidate failed to load" : error;
        return t;
    }
    t.created = true;

    KnobTrialPolicy pol;
    pol.window_tokens = sc.trial.window_tokens;
    CalibrationTurn turn;
    turn.replay = run.replay;
    turn.reference_lo = run.reference_lo;
    turn.reference_hi = run.reference_hi;
    GenerateRequest req;
    req.prompt = k_calibration_prompt;
    // The most a timing can take; it ends as soon as the rate is known.
    req.n_predict = RateProbe(true, true, pol).tokens_max();
    req.clear_kv = true;
    req.render_text = false;
    req.stop = stop;
    req.calibration = &turn;
    const RunResult r = s->generate(req);
    if (r.cancelled || (stop && stop->load(std::memory_order_relaxed))) {
        t.aborted = true;
        return t;
    }
    if (!r) {
        // A request the session refused (a context too short for the instruction) says nothing about
        // the placement: no rate, and the side is not marked. A decode that failed is as unusable here
        // as a load that did.
        if (!run_error_recoverable(r.error_kind)) t.created = false;
        t.error = r.error;
        return t;
    }
    t.tokens_per_second = turn.tokens_per_second;
    t.step_lo = turn.step_lo;
    t.step_hi = turn.step_hi;
    t.windows = turn.windows;
    t.cut = turn.cut;
    t.tokens = std::move(turn.tokens);
    return t;
}

} // namespace bmoe
