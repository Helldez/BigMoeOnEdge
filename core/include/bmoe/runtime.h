// The engine entry point: compose model + streaming + generation from a RunConfig.
//
// run() loads the model with the layout the streamer requires (mmap on, no weight
// repack), discovers the MoE expert tensors through a one-token capture warm-up, binds
// them to the expert source, then greedily generates n_predict tokens — reporting each
// token to the optional callback/sink and returning a RunSummary. Greedy sampling makes
// the output a deterministic function of the graph, which is what the byte-identity
// gates rely on — true of every configuration except MoeStreamConfig::drop_cold_frac, which
// decides from live cache state and so is reproducible only within a single run.
#pragma once

#include "bmoe/chat.h"
#include "bmoe/run_error.h"
#include "bmoe/config.h"
#include "bmoe/metrics.h"

#include <functional>
#include <string>
#include <vector>

namespace bmoe {

class IRouteTraceSink;
class IComputeTraceSink;
class IIoTraceSink;

struct RunResult {
    bool ok = false;
    bool cancelled = false; // generation was interrupted by Session::cancel() (ok stays true)
    std::string error;
    RunError error_kind = RunError::None; // set with `error`; what a caller decides on
    std::string generated_text;
    // The reasoning span, when a thinking model's chat template separated it from the answer.
    // Empty otherwise (chat off, non-reasoning model, harmony no-think). Display-only; the answer
    // in generated_text already has it stripped. See TokenMetrics::reasoning.
    std::string reasoning_text;
    // The tool calls the turn made, parsed out of the generation by the model's own chat format.
    // Empty unless the request described tools. A turn that calls usually has no answer text.
    std::vector<ChatToolCall> tool_calls;
    RunSummary summary;
    explicit operator bool() const { return ok; }
};

// Where a run reports to. None of it is configuration: it changes what a front-end is told, never
// what is generated, which is why it is not in RunConfig (and not in the parameter table).
struct RunOutputs {
    // Invoked once per generated token before the next decode. Nullable.
    std::function<void(const TokenMetrics &)> on_token;
    // Fill TokenMetrics::text and ::reasoning on every token. Only a reader of the per-token answer
    // (a line protocol, a live view) wants it: it re-parses the whole generation each token. The
    // final answer is parsed once at the end either way.
    bool render_text = false;
    IMetricsSink * sink = nullptr; // the same per-token metrics plus the final summary
    // Diagnostics that perturb what they measure — see bmoe/route_trace.h and bmoe/decode_trace.h.
    IRouteTraceSink * route_trace = nullptr;
    IComputeTraceSink * compute_trace = nullptr;
    IIoTraceSink * io_trace = nullptr;
};

// Run one generation. Blocks until generation completes or errors.
RunResult run(const RunConfig & cfg, const RunOutputs & out = {});

} // namespace bmoe
