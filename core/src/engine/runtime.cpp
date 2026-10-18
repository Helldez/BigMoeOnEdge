#include "bmoe/runtime.h"
#include "bmoe/session.h"

#include <string>

namespace bmoe {

const char * run_error_name(RunError e) {
    switch (e) {
    case RunError::None:
        return "none";
    case RunError::ContextFull:
        return "context_full";
    case RunError::EmptyPrompt:
        return "empty_prompt";
    case RunError::TemplateRejected:
        return "template_rejected";
    case RunError::NeedsChatMode:
        return "needs_chat_mode";
    case RunError::InvalidRequest:
        return "invalid_request";
    case RunError::Failed:
        return "failed";
    }
    return "failed";
}

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
    sc.n_gpu_layers = cfg.n_gpu_layers;   // the first stage's placement; 0 = everything on the host
    sc.buft_overrides = cfg.buft_overrides;
    sc.device_use = cfg.device_use;
    sc.n_threads_batch = cfg.n_threads_batch;
    sc.sampling = cfg.sampling; // greedy by default; opt-in stochastic decoding
    sc.moe = cfg.moe;
    sc.spec = cfg.spec;       // self-speculation (MTP head or n-gram lookup); off by default
    sc.prefill = cfg.prefill; // prefill on an accelerator; off by default
    sc.decide = cfg.decide;
    return sc;
}

// run() is the one-shot convenience: open a Session, generate once, close. The engine's real
// state (model, context, warm expert cache) lives in Session (session.cpp); keeping run() as a
// thin wrapper means the byte-identity gates exercise the exact same open/generate machinery an
// interactive session uses.
RunResult run(const RunConfig & cfg, const RunOutputs & out) {
    ValidationResult v = validate(cfg);
    if (!v) {
        RunResult r;
        r.error = v.error;
        r.error_kind = RunError::Failed;
        return r;
    }

    const SessionConfig sc = session_config_from(cfg);

    std::string error;
    std::unique_ptr<Session> session = Session::open(sc, error, out.route_trace, out.compute_trace, out.io_trace);
    if (!session) {
        RunResult r;
        r.error = error;
        r.error_kind = RunError::Failed;
        return r;
    }

    GenerateRequest req;
    req.prompt = cfg.prompt;
    req.n_predict = cfg.n_predict;
    req.think = cfg.think;
    req.clear_kv = true;
    // A plain front-end writes `piece` as it goes and a benchmark run reads nothing: building the
    // per-token answer for them would be O(n²) of nothing.
    req.render_text = out.render_text;

    return session->generate(req, out.on_token, out.sink);
}

} // namespace bmoe
