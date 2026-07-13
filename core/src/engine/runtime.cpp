#include "bmoe/runtime.h"
#include "bmoe/session.h"

#include <string>

namespace bmoe {

// run() is the one-shot convenience: open a Session, generate once, close. The engine's real
// state (model, context, warm expert cache) lives in Session (session.cpp); keeping run() as a
// thin wrapper means the byte-identity gates exercise the exact same open/generate machinery an
// interactive session uses. n_batch = n_ctx so the whole prompt still prefills in one batch, and
// n_ctx is passed through untouched so the gates run at exactly the context they specify.
RunResult run(const RunConfig & cfg, const std::function<void(const TokenMetrics &)> & on_token, IMetricsSink * sink) {
    ValidationResult v = validate(cfg);
    if (!v) {
        RunResult r;
        r.error = v.error;
        return r;
    }

    SessionConfig sc;
    sc.model_path = cfg.model_path;
    sc.n_threads = cfg.n_threads;
    sc.n_ctx = cfg.n_ctx;
    sc.n_batch = cfg.n_ctx; // one-batch prefill for any prompt that fits the context
    sc.chatml = cfg.chatml;
    sc.moe = cfg.moe;

    std::string error;
    std::unique_ptr<Session> session = Session::open(sc, error);
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

    return session->generate(req, on_token, sink);
}

} // namespace bmoe
