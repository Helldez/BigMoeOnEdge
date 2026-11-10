// The drafter source end to end: a real session over the tiny MoE fixture, streamed, drafting with
// a tiny draft model in a file of its own (scripts/make-tiny-moe.py --arch dflash).
//
// The drafter's weights are random, so its drafts are noise and almost none is accepted. That is the
// point of the fixture, not a limit of it: every step then takes the whole path a wrong draft takes
// (a second model loaded, the target's hidden states moved to it, a block drafted, a wide verify
// decode, the rejected tail rolled back out of the KV cache), which is the half of the loop a
// mistake corrupts silently. There is no reference text to compare against, for the reason
// docs/mtp.md gives for every draft source: a batched verify is not bit-identical to single-token
// decodes. What is asserted is that the machinery holds: the kind of the file is read from its
// header, the run drafts, generates every token it was asked for, and does the same again on a
// second turn of the same session.
//
//   bmoe_drafter_session_test <tiny-moe-qwen3moe.gguf> <tiny-moe-drafter.gguf>
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
    cfg.moe.enabled = true; // streamed experts, cache off: every verify position reads its own
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
} // namespace

int main(int argc, char ** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <tiny-moe-qwen3moe.gguf> <tiny-moe-drafter.gguf>\n", argv[0]);
        return 2;
    }
    const std::string model = argv[1];
    const std::string drafter = argv[2];
    std::string err;

    // The unspeculated run: how many tokens this prompt generates on this model.
    int n_plain = 0;
    {
        std::unique_ptr<bmoe::Session> s = bmoe::Session::open(config(model), err);
        if (!s) {
            std::fprintf(stderr, "plain session failed to open: %s\n", err.c_str());
            return 2;
        }
        const bmoe::RunResult r = s->generate(request());
        if (!r) {
            std::fprintf(stderr, "plain run failed: %s\n", r.error.c_str());
            return 2;
        }
        n_plain = r.summary.n_generated;
        check(n_plain > 0, "the fixture generates without a drafter", std::to_string(n_plain) + " tokens");
    }

    // The drafter: drafts on every step, and the run still yields every token.
    {
        bmoe::SessionConfig cfg = config(model);
        cfg.spec.source = bmoe::DraftSource::drafter;
        cfg.spec.drafter_file = drafter;
        cfg.spec.draft_max = 3;
        std::unique_ptr<bmoe::Session> s = bmoe::Session::open(cfg, err);
        check(s != nullptr, "a draft model's file opens beside the model it was made for", err);
        if (s) {
            const bmoe::RunResult r1 = s->generate(request());
            check(static_cast<bool>(r1), "a drafted turn completes", r1.error);
            const bmoe::RunSummary & a = r1.summary;
            check(a.mtp_drafted > 0 && a.drafted_steps > 0, "the drafter proposes tokens", counts(a));
            check(a.mtp_accepted >= 0 && a.mtp_accepted <= a.mtp_drafted && a.mtp_decodes > 0 &&
                      a.mtp_decodes <= a.n_generated,
                  "accepted drafts and verify decodes are within what was drafted and generated", counts(a));
            check(a.n_generated == n_plain && !r1.generated_text.empty(),
                  "every token asked for is generated, whatever was rejected", counts(a));

            // The same turn again: the draft context and the target were both rewound at the end of
            // the first, so the second starts from the same state and takes the same steps.
            const bmoe::RunResult r2 = s->generate(request());
            check(static_cast<bool>(r2), "a second drafted turn completes", r2.error);
            check(r2.generated_text == r1.generated_text && r2.summary.mtp_drafted == a.mtp_drafted &&
                      r2.summary.mtp_accepted == a.mtp_accepted && r2.summary.mtp_decodes == a.mtp_decodes,
                  "a second turn of the session repeats the first", counts(r2.summary));
        }
    }

    // What a file is comes from its header, so a file that is not a draft is refused by name of the
    // parameter that named it, at load.
    {
        bmoe::SessionConfig cfg = config(model);
        cfg.spec.source = bmoe::DraftSource::drafter;
        cfg.spec.drafter_file = model; // a model, not a draft for one
        err.clear();
        std::unique_ptr<bmoe::Session> s = bmoe::Session::open(cfg, err);
        check(!s && err.find("drafter-file") != std::string::npos, "a model given as the draft model is refused", err);
    }
    {
        bmoe::SessionConfig cfg = config(model);
        cfg.spec.source = bmoe::DraftSource::mtp;
        cfg.spec.mtp_head = drafter; // a drafter, not this model's head
        err.clear();
        std::unique_ptr<bmoe::Session> s = bmoe::Session::open(cfg, err);
        check(!s && err.find("mtp-head") != std::string::npos, "a draft model given as the MTP head is refused", err);
    }

    std::printf("%s\n", failures ? "FAILED" : "all drafter checks passed");
    return failures ? 1 : 0;
}
