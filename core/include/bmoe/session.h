// A persistent engine session: load the model and warm the expert cache ONCE, then serve
// many prompts against that resident state.
//
// run() (runtime.h) is the one-shot convenience — it opens a Session, generates once, and
// closes. Session exists for the interactive case: a fresh process per prompt re-pays the
// model load (tens of seconds) and the whole expert-cache warm-up ramp every time, which is
// exactly what streaming was meant to avoid. Keeping the process alive amortises both:
//
//   auto s = Session::open(cfg, err);          // model load + capture + source.init, once
//   s->generate({.prompt = "..."});            // prefill + decode; cache survives
//   s->generate({.prompt = "..."});            // starts warm — no reload, no cold ramp
//
// This header is pure policy (no llama.cpp types) — the native state lives behind a pimpl,
// so the CLI and tests link it without pulling in the backend headers.
#pragma once

#include "bmoe/config.h"
#include "bmoe/metrics.h"
#include "bmoe/runtime.h"

#include <functional>
#include <memory>
#include <string>

namespace bmoe {

class IRouteTraceSink;
class IComputeTraceSink;
class IIoTraceSink;

// Everything fixed for the model's lifetime — set once at open(). n_ctx and n_batch are
// baked into the llama context at creation and cannot change per prompt, so size them for
// the longest prompt+generation the session will serve.
struct SessionConfig {
    std::string model_path;
    int n_threads = 4;
    int n_ctx = 2048;
    int n_batch = 512; // prefill chunk capacity; longer prompts are prefilled in n_batch slices
    bool chatml = false;
    // Active-expert (top-k) override applied at load via a kv_override on the arch-prefixed
    // expert_used_count key. 0 = use the model's own count. See RunConfig::n_expert_used.
    int n_expert_used = 0;
    // Compute-trace granularity: false = a barrier per graph node, true = per layer boundary.
    // Only read when a compute-trace sink is attached. See RunConfig::compute_trace_layers.
    bool compute_trace_layers = false;
    SamplingConfig sampling; // fixed for the session; greedy by default (temp <= 0)
    MoeStreamConfig moe;
};

// The RunConfig → SessionConfig mapping, in one place. Both entry points that open a session from a
// RunConfig — run() and the CLI's interactive loop — need it, and they used to spell it out field by
// field. Two copies of a mapping is one copy too many: adding a field to RunConfig must not depend on
// remembering to touch both. n_batch = n_ctx so any prompt that fits the context prefills in one batch.
SessionConfig session_config_from(const RunConfig & cfg);

// How a "no thinking" request can actually be honoured for the loaded model. Setting the
// template's enable_thinking kwarg is only a request; templates that never read it discard it
// silently (issue #82). Decided once at open() by observing the template, never by model name.
enum class ThinkControl {
    Template,    // the template renders differently with thinking off — honoured at the source
    ForcedFinal, // harmony/gpt-oss shape: the final channel is primed in the prompt instead
    Sampler,     // the template ignores the flag, but the model's thinking tags let the engine
                 // force the block closed while decoding
    None,        // the flag is ignored and no tags are exposed: the request cannot be honoured
};

// Stable wire/display name for a control mode: "template" | "forced_final" | "sampler" | "none".
// Part of the CLI's line protocol (docs/telemetry.md), so these strings are a contract.
const char * think_control_name(ThinkControl c);

// Per-prompt request. clear_kv=true (the default) makes each prompt independent while the
// expert cache stays warm; clear_kv=false continues the KV cache for multi-turn chat.
struct GenerateRequest {
    std::string prompt;
    int n_predict = 32;
    bool think = true;
    bool clear_kv = true;
    // Cap on the tokens the model may spend inside one reasoning block; -1 leaves it
    // unlimited. Enforced while decoding, so it applies even to templates that ignore
    // `think`. Only meaningful with chatml. See RunConfig::reasoning_budget.
    int reasoning_budget = -1;
};

class Session {
public:
    ~Session();

    // Load the model, discover the MoE expert tensors, and initialise the expert source.
    // Returns nullptr and sets `error` on failure. The returned session owns all native
    // state and must outlive every generate() call.
    //
    // The trace sinks (all nullable) turn on their diagnostic for every generate() on this session
    // and must outlive it. None is ever on for a benchmark run — each perturbs what it measures.
    //   * route_trace   — per-step, per-layer routing; ignored when streaming is off (no routing to
    //                     observe). See bmoe/route_trace.h.
    //   * compute_trace — per-node compute and fault attribution. Works with or without streaming,
    //                     so a dense mmap baseline can be compared against a streamed run.
    //   * io_trace      — per-read flash latency/size; ignored when streaming is off (no reads).
    // See bmoe/decode_trace.h for the latter two.
    static std::unique_ptr<Session> open(const SessionConfig & cfg,
                                         std::string & error,
                                         IRouteTraceSink * route_trace = nullptr,
                                         IComputeTraceSink * compute_trace = nullptr,
                                         IIoTraceSink * io_trace = nullptr);

    // Generate one response. Serialized: one generation at a time per session. `on_token`
    // and `sink` receive the same per-token metrics as run(). Cache state carries over from
    // the previous call. If cancel() fired, returns ok=true with cancelled=true.
    RunResult generate(const GenerateRequest & req,
                       const std::function<void(const TokenMetrics &)> & on_token = nullptr,
                       IMetricsSink * sink = nullptr);

    // Request the in-flight generation to stop. Thread-safe; may be called from another
    // thread while generate() runs. Takes effect at the next decode boundary via the abort
    // callback and leaves the model/cache intact for the next generate().
    void cancel();

    double load_seconds() const;      // model load + streaming setup, measured once at open()
    const std::string & arch() const; // model architecture ("qwen3moe", "gemma4", …)
    int n_ctx() const;

    // Which mechanism honours GenerateRequest::think for this model. Callers surface it so a
    // user is told when the control is real; None means the request cannot be honoured at all.
    // Template when chat is off — the raw-prompt path has no template to disagree with.
    ThinkControl think_control() const;

    // Set the expert-cache budget in MiB and evict down to it now. PRECONDITION: no generate() in
    // flight — call it between generations (e.g. from an app's memory-pressure callback). A no-op
    // when the cache is off. The only way the budget moves after init sizes it.
    void set_cache_budget_mb(int mib);

private:
    Session();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bmoe
