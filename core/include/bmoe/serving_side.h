// Which of two processors serves this machine's requests sooner, from each one's own timing of a model.
//
// A model held whole on a device that shares the host's memory can run there or on the cores, and the
// question has one answer only for a given request: its decoded tokens at each side's one-token rate,
// and its prompt at each side's rate as wide as a prompt is computed. The two rates do not move
// together: a processor with wide parallel units is slow one token at a time and fast on a wide graph,
// and the cores are the other way round, so neither rate alone ranks the sides.
//
// One rule, used by the load that times both sides of a model (engine/device_answer.h) and by every
// plan made afterwards on what that load recorded (planner.cpp, choose_shared_device). Two rules over
// two instruments gave opposite answers on the same machine: the load sent a model to the cores and
// the next plan sent it back.
//
// Pure policy: no backend, device, architecture or quantization appears here.
#pragma once

namespace bmoe {

// A side's speed on one model, in tokens per second; 0 where that rate was not measured.
struct SideRates {
    double decode_tps = 0.0; // one token at a time, as an answer is decoded
    double prompt_tps = 0.0; // a prompt's tokens, computed as wide as a prompt is
};

// A request's cost on each side, per decoded token: that token, plus the `prompt_per_token` prompt
// tokens a request brings with each decoded one. The prompt is weighed only where both sides have a
// prompt rate and the request has a prompt; otherwise the decode alone decides.
struct SideWeighing {
    bool weighed = false;      // both sides had a decode rate
    bool cores_sooner = false; // the cores serve a request faster than the device by the margin
    double device_s = 0.0, cores_s = 0.0;
};

// The device is the side in force: the cores take the model only where a request is shorter there by
// `margin` (a fraction; below 0 it is 0), because a reopening is paid for and a few tokens' timing is
// not a whole answer's.
SideWeighing weigh_sides(const SideRates & device, const SideRates & cores, double prompt_per_token, double margin);

} // namespace bmoe
