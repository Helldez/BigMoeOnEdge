// A load-time calibration's candidate, run by the engine (bmoe/session.h, run_calibration_candidate):
// a session opened for the candidate's config, one short timed answer, closed.

#include "bmoe/session.h"

#include "bmoe/log.h"

#include "../io/platform_io.h"
#include "../moe/gguf_offsets.h"
#include "knob_trial.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

// The most of the memory the system can still give that the model's files may take for every
// candidate to start with all of them cached: half. The other half is for what a candidate allocates
// itself, a copy of the experts in a device's host memory or of the dense part, which can be as large
// as the files; past it the system drops cached pages to make room, and the candidates no longer start
// alike.
constexpr double k_cached_model_share = 0.5;

// Whether the system can keep every one of `paths` cached through a calibration: their bytes against
// the memory it can still give (and a container still lets this process take), at k_cached_model_share.
bool files_fit_cache(const std::vector<std::string> & paths, uint64_t & bytes) {
    bytes = 0;
    for (const std::string & p : paths) {
        std::error_code ec;
        const uintmax_t n = std::filesystem::file_size(p, ec);
        if (ec) return false;
        bytes += (uint64_t) n;
    }
    uint64_t room = pio::mem_available_bytes();
    const uint64_t contained = pio::cgroup_memory_left();
    if (contained > 0 && (room == 0 || contained < room)) room = contained;
    return room > 0 && (double) bytes <= (double) room * k_cached_model_share;
}

// Every candidate starts from the same system cache of the model's files, so the later side is not
// measured on a warmer machine than the earlier one.
//
// Where the system can keep the whole model cached (files_fit_cache), every candidate starts with all
// of it cached: the first reads the files through once, and the ones after find them as it left them.
// That is also how the served load finds them, the same model in the same memory. Dropping the cache
// instead made every candidate read the whole model from the drive again: a load reads all of a model
// that is not streamed, so on a rented card with GLM-4.7 (130 GB, 500 GB of memory) each candidate paid
// the drive's pass over the files before its first token.
//
// Where it cannot, the cache of every shard (and the files beside the model) is handed back where the
// system takes the advice (pio::drop_file_cache), and then the dense part, the tensors every token reads
// whatever the placement, is read in on every system alike: each candidate opens with exactly the dense
// part cached, where pio::drop_file_cache is a no-op as where it is not. What stays uneven where the
// cache cannot be dropped is the experts a placement loads through the mapping: the later candidate may
// find some of the earlier one's still cached. They are not read in, since a model larger than the
// memory is what this engine exists for.
void even_model_file_cache(const RunConfig & cfg, int candidate, const std::atomic<bool> * stop) {
    const GgufOffsets offs = read_gguf_offsets(cfg.model_path.c_str());
    std::vector<std::string> beside;
    for (const std::string * p : {&cfg.mmproj, &cfg.spec.mtp_head})
        if (!p->empty()) beside.push_back(*p);
    if (offs.ok) {
        std::vector<std::string> all = offs.shard_paths;
        all.insert(all.end(), beside.begin(), beside.end());
        uint64_t bytes = 0;
        if (files_fit_cache(all, bytes)) {
            if (candidate > 0) return; // the first one cached them
            log_printf(LogLevel::Info,
                       "calibration: the model's files (%.1f GiB) fit the system's cache: every candidate starts "
                       "with them cached",
                       (double) bytes / (1024.0 * 1024.0 * 1024.0));
            for (const std::string & p : all) {
                if (stop && stop->load(std::memory_order_relaxed)) return;
                std::error_code ec;
                const uintmax_t n = std::filesystem::file_size(p, ec);
                if (!ec) pio::read_through_cache(p.c_str(), {{0, (uint64_t) n}}, stop);
            }
            return;
        }
    }
    for (const std::string & p : beside)
        pio::drop_file_cache(p.c_str());
    if (!offs.ok) {
        pio::drop_file_cache(cfg.model_path.c_str());
        return;
    }
    for (const std::string & p : offs.shard_paths)
        pio::drop_file_cache(p.c_str());
    const GgufModelInfo info = read_gguf_model_info(cfg.model_path.c_str());
    const ExpertLayout layout = discover_expert_layout(offs.extents, info.n_expert);
    // Without the experts told apart, the dense part is the whole file: never read a model whole.
    if (!layout.ok) return;
    // The dense tensors' byte ranges, per shard, in file order.
    std::vector<std::vector<std::pair<uint64_t, uint64_t>>> ranges(offs.shard_paths.size());
    for (const TensorExtent & t : offs.extents) {
        int il = -1;
        if (layout.slot_of(t.name.c_str(), il) >= 0) continue;
        const auto off = offs.off_by_name.find(t.name), size = offs.size_by_name.find(t.name);
        const auto file = offs.file_by_name.find(t.name);
        if (off == offs.off_by_name.end() || size == offs.size_by_name.end() || file == offs.file_by_name.end())
            continue;
        if (file->second >= 0 && (size_t) file->second < ranges.size())
            ranges[(size_t) file->second].push_back({off->second, size->second});
    }
    for (size_t f = 0; f < ranges.size(); ++f) {
        if (stop && stop->load(std::memory_order_relaxed)) return; // a waiting request does not wait through this
        std::sort(ranges[f].begin(), ranges[f].end());
        pio::read_through_cache(offs.shard_paths[f].c_str(), ranges[f], stop);
    }
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

    even_model_file_cache(cfg, run.index, stop);
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
