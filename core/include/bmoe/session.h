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
    MoeStreamConfig moe;
};

// Per-prompt request. clear_kv=true (the default) makes each prompt independent while the
// expert cache stays warm; clear_kv=false continues the KV cache for multi-turn chat.
struct GenerateRequest {
    std::string prompt;
    int n_predict = 32;
    bool think = true;
    bool clear_kv = true;
};

class Session {
public:
    ~Session();

    // Load the model, discover the MoE expert tensors, and initialise the expert source.
    // Returns nullptr and sets `error` on failure. The returned session owns all native
    // state and must outlive every generate() call.
    //
    // `route_trace` (nullable) turns on the per-step, per-layer routing trace for every
    // generate() on this session and must outlive it — a diagnostic, ignored when streaming is
    // off, and never on for a benchmark run. See bmoe/route_trace.h.
    static std::unique_ptr<Session>
    open(const SessionConfig & cfg, std::string & error, IRouteTraceSink * route_trace = nullptr);

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

    // Set the expert-cache budget in MiB and evict down to it now. PRECONDITION: no generate() in
    // flight — call it between generations (e.g. from an app's memory-pressure callback). A no-op
    // when the cache is off. Complements --cache-mb auto's automatic tracking with explicit control.
    void set_cache_budget_mb(int mib);

private:
    Session();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bmoe
