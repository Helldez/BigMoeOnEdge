#include "bmoe/runtime.h"
#include "bmoe/session.h"

#include <string>

namespace bmoe {

// n_ctx is passed through untouched so the gates run at exactly the context they specify.
SessionConfig session_config_from(const RunConfig & cfg) {
    SessionConfig sc;
    sc.model_path = cfg.model_path;
    sc.n_threads = cfg.n_threads;
    sc.n_ctx = cfg.n_ctx;
    sc.n_batch = cfg.n_ctx;     // one-batch prefill for any prompt that fits the context
    sc.n_ubatch = cfg.n_ubatch; // 0 = follow n_batch; smaller trades prefill speed for memory
    sc.chatml = cfg.chatml;
    sc.n_expert_used = cfg.n_expert_used; // active-expert (top-k) override; 0 = model default
    sc.compute_trace_layers = cfg.compute_trace_layers;
    sc.sampling = cfg.sampling; // greedy by default; opt-in stochastic decoding
    sc.moe = cfg.moe;
    sc.spec = cfg.spec; // self-speculation (MTP head or n-gram lookup); off by default
    return sc;
}

// run() is the one-shot convenience: open a Session, generate once, close. The engine's real
// state (model, context, warm expert cache) lives in Session (session.cpp); keeping run() as a
// thin wrapper means the byte-identity gates exercise the exact same open/generate machinery an
// interactive session uses.
RunResult run(const RunConfig & cfg,
              const std::function<void(const TokenMetrics &)> & on_token,
              IMetricsSink * sink,
              IRouteTraceSink * route_trace,
              IComputeTraceSink * compute_trace,
              IIoTraceSink * io_trace) {
    ValidationResult v = validate(cfg);
    if (!v) {
        RunResult r;
        r.error = v.error;
        return r;
    }

    const SessionConfig sc = session_config_from(cfg);

    std::string error;
    std::unique_ptr<Session> session = Session::open(sc, error, route_trace, compute_trace, io_trace);
    if (!session) {
        RunResult r;
        r.error = error;
        return r;
    }

    GenerateRequest req;
    req.prompt = cfg.prompt;
    req.n_predict = cfg.n_predict;
    req.think = cfg.think;
    req.clear_kv = true;
    // Only --progress reads the per-token parsed answer; the plain path writes `piece` as it goes,
    // and a benchmark run reads neither. Building it re-parses the whole generation every token, so
    // an unread one is O(n²) of nothing. The final answer is parsed once at the end either way.
    req.render_text = cfg.progress;

    return session->generate(req, on_token, sink);
}

} // namespace bmoe
