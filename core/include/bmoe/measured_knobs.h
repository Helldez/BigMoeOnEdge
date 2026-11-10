// What a session measured about this model's lossless knobs on this machine, kept between loads.
//
// The decode thread count and whether reads overlap compute change no output, and which setting
// decodes faster is a property of this machine running this model, not something a rule over
// either can predict: the thread rule from core classes was shipped and withdrawn in a day, and
// overlap measured +5% on one phone and +8-14% on one laptop with nothing in the plan able to say
// where it stops paying. So the first real request of a session measures them, interleaved on the
// user's own answer (RunConfig::trial), and hands the winner back as a value. The front-end keeps it
// against the model file's fingerprint, beside the model's routing; the next plan of the same model
// on the same machine and engine starts from it and measures nothing. See
// docs/measured-planning.md (Step 2).
//
// Pure: no llama.cpp, no I/O, no clock. The text format is a string in and a string out; where it
// is stored is the front-end's decision.
#pragma once

#include "bmoe/hardware_profile.h"

#include <string>
#include <vector>

namespace bmoe {

struct MeasuredKnobs {
    // Whose measurement this is: the machine (hardware_fingerprint) and the engine version
    // (bmoe::version()) it was measured with. The model is the file it is kept under.
    std::string hardware;
    std::string engine;

    // The decode thread count that won, 0 when not measured. `threads_tried` lists every count the
    // trial decoded with, the incumbent first; `threads_gain` is how much faster the winner decoded
    // than the incumbent, or, when the incumbent held, the best candidate's figure (below the margin).
    int threads = 0;
    std::vector<int> threads_tried;
    double threads_gain = 0.0;

    // Overlap: -1 not measured, 0 off, 1 on. `overlap_incumbent` is the setting the trial started
    // from, and `overlap_gain` how much faster the other one decoded than it (below the margin: the
    // incumbent held).
    int overlap = -1;
    int overlap_incumbent = -1;
    double overlap_gain = 0.0;

    int rounds = 0; // timed windows per candidate behind each verdict

    // The device tier, decided across loads rather than inside one: a device expert cache is fixed
    // when a context is created, so the two placements cannot alternate within an answer. A load that
    // arms the cache measures its decode rate; the next load runs the experts where the capacity
    // fitter put them and measures that; the plan after keeps the faster (planner.cpp,
    // choose_device_tier). Tokens per second, 0 when that side was not measured. A rule could not
    // tell the two apart: a cache holding a tenth of a 355B model's experts on a PCIe 3 link decoded
    // at 1.6 tok/s where the fitter's placement did 3.9, and a cache on a 12 GB card won 17.5 to 9.9.
    double device_cache_tps = 0.0;
    double no_device_cache_tps = 0.0;

    // The graph width, decided across loads where a wide graph's compute buffers take device memory
    // the experts would use (planner.cpp, choose_graph_width). A wide graph prefills a long prompt in
    // one pass; a narrow one leaves the experts more of the card. Which wins is a property of this
    // machine AND of how it is used: on two 24 GB cards with GLM-4.5-Air the narrow graph decoded 37%
    // faster, and the wide one prefilled a 9.5k-token prompt several times faster. So each load times
    // both halves, and the plan weighs them by the requests this machine actually serves.
    // `width_narrow` is the narrow width compared (0: none); the rates are tokens per second, the
    // prefill ones over prompts longer than `width_narrow` only (where the width makes a difference),
    // 0 when that side or that kind of prompt was not seen.
    int width_narrow = 0;
    double wide_decode_tps = 0.0, narrow_decode_tps = 0.0;
    double wide_prefill_tps = 0.0, narrow_prefill_tps = 0.0;
    // What the requests of the last load that measured a side looked like: per request, the tokens
    // decoded and the prompt tokens beyond the narrow width (prefilled after reuse), over `usage_requests`.
    double usage_gen_tokens = 0.0, usage_long_prompt_tokens = 0.0;
    int usage_requests = 0;

    bool empty() const { return verdicts() == 0; }
    // How many measurements this record carries: a front-end keeps a record again when it grows. The
    // requests a width trial has seen count too, so the usage it weighs the two widths by is kept as it
    // grows rather than as the first request left it.
    int verdicts() const {
        return (threads > 0) + (overlap >= 0) + (device_cache_tps > 0.0) + (no_device_cache_tps > 0.0) +
               (wide_decode_tps > 0.0) + (narrow_decode_tps > 0.0) + (wide_prefill_tps > 0.0) +
               (narrow_prefill_tps > 0.0) + usage_requests;
    }
};

// The hardware facts a stored measurement is valid for, as 16 hex digits: the core count and
// classes, the host's total memory, and each compute device's name and own memory. Facts that do
// not move between loads, never a rate or a free-memory figure: what was measured about the
// machine stays valid when the memory that happened to be free does not.
std::string hardware_fingerprint(const HardwareProfile & hw);

// The records kept for one model, one line per (machine, engine), and back. False with `error` set
// when the text is not this format.
std::string format_measured_knobs(const std::vector<MeasuredKnobs> & records);
bool parse_measured_knobs(const std::string & text, std::vector<MeasuredKnobs> & out, std::string & error);

// Put `rec` in `records`, merged into the one of the same machine and engine: each measurement `rec`
// carries replaces that one, and what it does not carry is kept. A load measures what its plan left
// unmeasured, so the record of the load before holds the rest.
void keep_measured_knobs(std::vector<MeasuredKnobs> & records, const MeasuredKnobs & rec);

// The record of this machine and engine, or null.
const MeasuredKnobs * find_measured_knobs(const std::vector<MeasuredKnobs> & records,
                                          const std::string & hardware,
                                          const std::string & engine);

} // namespace bmoe
