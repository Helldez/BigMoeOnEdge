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

    bool empty() const { return threads <= 0 && overlap < 0; }
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

// Put `rec` in `records`, replacing the one of the same machine and engine.
void keep_measured_knobs(std::vector<MeasuredKnobs> & records, const MeasuredKnobs & rec);

// The record of this machine and engine, or null.
const MeasuredKnobs * find_measured_knobs(const std::vector<MeasuredKnobs> & records,
                                          const std::string & hardware,
                                          const std::string & engine);

} // namespace bmoe
