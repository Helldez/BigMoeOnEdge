// The self-draft source end to end: a real session over a tiny MoE fixture, streamed, the model
// drafting for itself with its router capped to fewer experts (llama_set_n_expert_used_cap) on a
// second sequence that shares the KV prefix and is dropped before every verify.
//
// What is asserted is the machinery, as for every draft source (docs/mtp.md: a batched verify is not
// bit-identical to single-token decodes, so there is no reference text): the run drafts, accepts some
// of it (the draft is the same model, so it agrees with itself far more often than noise would),
// generates every token it was asked for, streams fewer experts per drafted token than per plain one
// (proof that the cap reached the graph), and a second turn of the same session repeats the first,
// which only holds if the draft's sequence left nothing behind in the target's KV or recurrent state.
// A width the model does not route below is refused at open, by the parameter's name.
//
// Run on a plain-attention fixture and on a hybrid one, whose recurrent state the draft's sequence
// copies on its first write.
//
//   bmoe_self_draft_session_test <tiny-moe.gguf> [<tiny-moe.gguf> ...]
#include "bmoe/session.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {
int failures = 0;

void check(bool ok, const std::string & name, const std::string & detail = {}) {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++failures;
}

constexpr int k_tokens = 24;

bmoe::SessionConfig config(const std::string & model) {
    bmoe::SessionConfig cfg;
    cfg.model_path = model;
    cfg.n_threads = 2;
    cfg.n_ctx = 256;
    cfg.n_batch = 128;
    cfg.n_ubatch = 64;
    cfg.device_use = bmoe::DeviceUse::CpuOnly;
    cfg.moe.enabled = true; // streamed experts, cache off: the narrow draft reads its own experts
    cfg.moe.cache_mb = 0;
    cfg.moe.io_threads = 2;
    return cfg;
}

bmoe::GenerateRequest request() {
    bmoe::GenerateRequest req;
    req.prompt = "The quick brown fox";
    req.n_predict = k_tokens;
    req.clear_kv = true;
    return req;
}

std::string counts(const bmoe::RunSummary & s) {
    return "tokens=" + std::to_string(s.n_generated) + " drafted=" + std::to_string(s.mtp_drafted) +
           " accepted=" + std::to_string(s.mtp_accepted) + " decodes=" + std::to_string(s.mtp_decodes) +
           " drafted_steps=" + std::to_string(s.drafted_steps);
}

void run_model(const std::string & model) {
    std::printf("-- %s\n", model.c_str());
    std::string err;

    int n_plain = 0;
    int n_used = 0;
    double plain_mib_per_token = 0.0;
    std::string plain_text;
    {
        std::unique_ptr<bmoe::Session> s = bmoe::Session::open(config(model), err);
        check(s != nullptr, "the plain session opens", err);
        if (!s) return;
        n_used = s->n_expert_used();
        const bmoe::RunResult r = s->generate(request());
        check(static_cast<bool>(r), "the plain run completes", r.error);
        n_plain = r.summary.n_generated;
        plain_text = r.generated_text;
        plain_mib_per_token = n_plain > 0 ? r.summary.moe_read_mib / n_plain : 0.0;
        check(n_plain > 0 && n_used > 1, "the fixture generates and routes to more than one expert",
              std::to_string(n_plain) + " tokens, " + std::to_string(n_used) + " experts per token");
        if (n_used <= 1) return;
    }

    {
        bmoe::SessionConfig cfg = config(model);
        cfg.spec.source = bmoe::DraftSource::self;
        cfg.spec.self_draft_experts = n_used - 1;
        cfg.spec.draft_max = 3;
        std::unique_ptr<bmoe::Session> s = bmoe::Session::open(cfg, err);
        check(s != nullptr, "a self-drafting session opens", err);
        if (s) {
            const bmoe::RunResult r1 = s->generate(request());
            check(static_cast<bool>(r1), "a self-drafted turn completes", r1.error);
            const bmoe::RunSummary & a = r1.summary;
            check(a.mtp_drafted > 0 && a.drafted_steps > 0, "the model drafts for itself", counts(a));
            check(a.mtp_accepted > 0 && a.mtp_accepted <= a.mtp_drafted && a.mtp_decodes > 0 &&
                      a.mtp_decodes < a.n_generated,
                  "some drafts are accepted, so a verify decode yields more than one token", counts(a));
            check(a.n_generated == n_plain && !r1.generated_text.empty(),
                  "every token asked for is generated, whatever was rejected", counts(a));
            // The narrow routing is real: on a tiny fixture the experts barely move the logits, so
            // acceptance alone cannot tell a capped draft from the full model. The bytes can: with the
            // cache off every decode streams the experts it routes to, and a draft routed to fewer
            // reads fewer per token.
            const double draft_mib_per_token = a.mtp_drafted > 0 ? a.mtp_draft_read_mib / a.mtp_drafted : 0.0;
            char bytes[160];
            std::snprintf(bytes, sizeof(bytes), "%.4f MiB per drafted token, %.4f per plain token", draft_mib_per_token,
                          plain_mib_per_token);
            check(draft_mib_per_token > 0.0 && draft_mib_per_token < 0.75 * plain_mib_per_token,
                  "a drafted token streams fewer experts than a plain one", bytes);
            std::printf("[INFO] text %s the plain run's\n",
                        r1.generated_text == plain_text ? "equals" : "differs from");

            const bmoe::RunResult r2 = s->generate(request());
            check(static_cast<bool>(r2), "a second self-drafted turn completes", r2.error);
            check(r2.generated_text == r1.generated_text && r2.summary.mtp_drafted == a.mtp_drafted &&
                      r2.summary.mtp_accepted == a.mtp_accepted && r2.summary.mtp_decodes == a.mtp_decodes,
                  "a second turn of the session repeats the first", counts(r2.summary));
        }
    }

    {
        bmoe::SessionConfig cfg = config(model);
        cfg.spec.source = bmoe::DraftSource::self;
        cfg.spec.self_draft_experts = n_used; // as wide as the model: no draft at all
        err.clear();
        std::unique_ptr<bmoe::Session> s = bmoe::Session::open(cfg, err);
        check(!s && err.find("self-draft-experts") != std::string::npos, "a self-draft as wide as the model is refused",
              err);
    }
}
} // namespace

int main(int argc, char ** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <tiny-moe.gguf> [<tiny-moe.gguf> ...]\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; ++i)
        run_model(argv[i]);
    std::printf("%s\n", failures ? "FAILED" : "all self-draft checks passed");
    return failures ? 1 : 0;
}
