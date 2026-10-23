// Real CPU decodes prove that Session applies the template grammar at both sampling sites and
// starts each request with fresh state. The tiny fixture has random weights and byte tokens:
// replay the emitted tokens through the same template grammar instead of assuming its first
// character is '{' (the template also permits whitespace and fenced JSON).
#include "bmoe/ngram_draft.h"
#include "bmoe/probe.h"
#include "bmoe/session.h"
#include "chat_messages.h"
#include "chat_parse.h"
#include "thinking_control.h"

#include "common.h"
#include "llama.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace {
int failures = 0;

void check(bool ok, const std::string & name, const std::string & detail = {}) {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++failures;
}

bmoe::SessionConfig config(const std::string & model, bool sampled) {
    bmoe::SessionConfig cfg;
    cfg.model_path = model;
    cfg.n_threads = 2;
    cfg.n_ctx = 2048;
    cfg.n_batch = 128;
    cfg.n_ubatch = 64;
    cfg.device_use = bmoe::DeviceUse::CpuOnly;
    cfg.chatml = true;
    if (sampled) {
        cfg.sampling.temp = 0.7f;
        cfg.sampling.seed = 42;
    }
    return cfg;
}

bmoe::GenerateRequest request(const std::string & schema, int tokens) {
    bmoe::GenerateRequest req;
    req.prompt = "x";
    req.n_predict = tokens;
    req.think = false;
    req.json_schema = schema;
    return req;
}

struct Captured {
    bmoe::RunResult result;
    std::vector<bmoe::TokenMetrics> rows;
    std::vector<std::string> pieces;
    uint64_t expert_bytes = 0;
};

Captured generate(bmoe::Session & session, const bmoe::GenerateRequest & req, bool cancel = false) {
    Captured captured;
    captured.result = session.generate(req, [&](const bmoe::TokenMetrics & m) {
        captured.rows.push_back(m);
        captured.pieces.push_back(m.piece);
        captured.expert_bytes += m.read_bytes;
        if (cancel) session.cancel();
    });
    return captured;
}

// No fixture token IDs are assumed. Load only its vocabulary, render the model's own template,
// and feed the pieces observed at the public Session callback through that grammar.
struct GrammarOracle {
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model{nullptr, llama_model_free};
    common_chat_templates_ptr templates;
    const llama_vocab * vocab = nullptr;
    std::unordered_map<std::string, llama_token> piece_tokens;
    bmoe::ThinkControl control = bmoe::ThinkControl::None;

    explicit GrammarOracle(const std::string & path) {
        auto params = llama_model_default_params();
        params.vocab_only = true;
        params.n_gpu_layers = 0;
        model.reset(llama_model_load_from_file(path.c_str(), params));
        if (!model) return;
        vocab = llama_model_get_vocab(model.get());
        for (llama_token token = 0; token < llama_vocab_n_tokens(vocab); ++token) {
            const auto piece = common_token_to_piece(vocab, token, true);
            if (!piece.empty()) piece_tokens.emplace(piece, token);
        }
        templates = common_chat_templates_init(model.get(), "");
        control = bmoe::detail::probe_think_control(templates.get());
    }

    common_chat_params render(const bmoe::GenerateRequest & req) const {
        const auto messages =
            req.messages.empty() ? std::vector<bmoe::ChatMessage>{{"user", req.prompt}} : req.messages;
        common_chat_templates_inputs inputs;
        bmoe::detail::build_turn_inputs(inputs, bmoe::detail::to_common_messages(messages), req.think, control);
        if (!req.messages.empty()) bmoe::detail::add_tools(inputs, req.tools, req.tool_choice);
        inputs.json_schema = req.json_schema;
        return common_chat_templates_apply(templates.get(), inputs);
    }

    std::vector<llama_token> tokens(const Captured & captured) const {
        std::vector<llama_token> out;
        for (const auto & piece : captured.pieces) {
            // Retokenizing a byte piece can normalize whitespace or an incomplete UTF-8
            // sequence. The fixture has a unique token for each byte, so invert detokenization.
            const auto found = piece_tokens.find(piece);
            if (found == piece_tokens.end()) return {};
            out.push_back(found->second);
        }
        return out;
    }

    bool prefix(const bmoe::GenerateRequest & req, const Captured & captured) const {
        if (!captured.result.ok || captured.pieces.empty() ||
            captured.pieces.size() != static_cast<size_t>(captured.result.summary.n_generated))
            return false;
        bmoe::detail::ToolGrammar grammar;
        std::string error;
        if (!grammar.init(render(req), vocab, error) || !grammar.active()) return false;
        const auto emitted = tokens(captured);
        if (emitted.size() != captured.pieces.size()) return false;
        for (const auto token : emitted) {
            if (!grammar.allows(token)) return false;
            grammar.accept(token);
            if (!grammar.active()) return false;
        }
        return true;
    }

    // Reconstruct exactly the n-gram proposals at each observed group boundary. Count an illegal
    // first unaccepted draft; such a draft must never be emitted even when the target likes it.
    // This does not claim which of target mismatch or grammar refusal ended the actual group.
    int refused_drafts(const bmoe::SessionConfig & cfg,
                       const bmoe::GenerateRequest & req,
                       const Captured & captured) const {
        const auto cp = render(req);
        std::vector<llama_token> context = common_tokenize(vocab, cp.prompt.str(), true, true);
        const auto emitted = tokens(captured);
        if (emitted.size() != captured.pieces.size()) return -1;
        bmoe::detail::ToolGrammar grammar;
        std::string error;
        if (!grammar.init(cp, vocab, error) || !grammar.active()) return -1;
        long long proposed = 0;
        int refused = 0;
        for (size_t start = 0; start < emitted.size();) {
            const int count = captured.rows[start].mtp_batch;
            if (count < 1 || start + count > emitted.size()) return -1;
            std::vector<llama_token> draft;
            const int room = req.n_predict - static_cast<int>(start) - 1;
            bmoe::ngram_draft(context, emitted[start], std::min(cfg.spec.draft_max, room), cfg.spec.ngram_min_match,
                              cfg.spec.ngram_max_match, draft);
            proposed += draft.size();
            for (int i = 0; i < count; ++i) {
                const auto token = emitted[start + i];
                if (i > 0 && (static_cast<size_t>(i - 1) >= draft.size() || token != draft[i - 1])) return -1;
                if (!grammar.allows(token)) return -1;
                grammar.accept(token);
                context.push_back(token);
            }
            if (static_cast<size_t>(count - 1) < draft.size() && !grammar.allows(draft[count - 1])) ++refused;
            start += count;
        }
        return proposed == captured.result.summary.mtp_drafted ? refused : -1;
    }
};

void lifecycle(const std::string & model, bool sampled, const GrammarOracle & oracle) {
    const std::string label = sampled ? "stochastic: " : "greedy: ";
    const std::string object_schema = R"({"type":"object"})";
    std::string error;
    auto session = bmoe::Session::open(config(model, sampled), error);
    check(session != nullptr, label + "the real session opens", error);
    if (!session) return;

    const auto plain_req = request("", 8);
    const auto reference = generate(*session, plain_req);
    check(reference.result.ok, label + "the ordinary reference runs", reference.result.error);

    for (const int budget : {1, 2}) {
        const auto req = request(object_schema, budget);
        const auto captured = generate(*session, req);
        check(oracle.prefix(req, captured) && !captured.result.cancelled &&
                  captured.result.summary.n_generated == budget,
              label + (budget == 1 ? "the first sampled token follows the template grammar"
                                   : "the later token follows the grammar and keeps a length stop"),
              captured.result.error);
    }
    for (const char * schema :
         {R"({"type":"array","items":{"type":"integer"}})", R"({"const":"answer"})", R"({"type":"integer"})"}) {
        const auto req = request(schema, 16);
        const auto captured = generate(*session, req);
        check(oracle.prefix(req, captured), label + "the session accepts schema " + schema, captured.result.error);
    }
    const auto plain = generate(*session, plain_req);
    check(plain.result.ok && reference.result.ok && plain.pieces == reference.pieces,
          label + "ordinary output after schemas matches the reset-RNG reference", plain.result.error);

    auto continued = request(object_schema, 1);
    continued.clear_kv = false;
    continued.messages = {{"user", "x"}};
    const auto before_error = generate(*session, continued);
    check(oracle.prefix(continued, before_error), label + "an explicit conversation reuses its prefix",
          before_error.result.error);
    for (const char * schema : {"{", R"({"type":"unsupported-type"})"}) {
        auto invalid = continued;
        invalid.json_schema = schema;
        const auto refused = session->generate(invalid);
        check(!refused.ok && bmoe::run_error_recoverable(refused.error_kind) &&
                  refused.error_kind != bmoe::RunError::InvalidOutput,
              label + "invalid schema is a recoverable client error", refused.error);
    }
    const auto after_error = generate(*session, continued);
    // The final prompt token is replayed to recover logits; the rest of the prefix must survive.
    check(oracle.prefix(continued, after_error) && after_error.result.summary.n_prompt == 1,
          label + "refused schemas preserve the previous conversation's reusable KV", after_error.result.error);

    auto combined = request(object_schema, 2);
    combined.messages = {{"user", "x"}};
    combined.tools = {{"read", "Read a file", R"({"type":"object"})"}};
    combined.think = true;
    const auto with_tools = generate(*session, combined);
    check(oracle.prefix(combined, with_tools), label + "thinking and tools reach the shared schema path",
          with_tools.result.error);
    auto required = combined;
    required.tool_choice = bmoe::ToolChoice::Required;
    const auto refused_required = session->generate(required);
    check(!refused_required.ok && refused_required.error_kind == bmoe::RunError::InvalidRequest,
          label + "a required tool conflicting with a response schema is explicitly refused", refused_required.error);
    const auto after_required = generate(*session, combined);
    check(oracle.prefix(combined, after_required), label + "automatic tools remain usable after that refusal",
          after_required.result.error);

    const auto ordinary_after_error = generate(*session, plain_req);
    check(ordinary_after_error.result.ok && reference.result.ok && ordinary_after_error.pieces == reference.pieces,
          label + "ordinary generation still works after schema errors and combined inputs",
          ordinary_after_error.result.error);

    const auto cancel_req = request(object_schema, 16);
    const auto cancelled = generate(*session, cancel_req, true);
    check(oracle.prefix(cancel_req, cancelled) && cancelled.result.cancelled && cancelled.rows.size() == 1,
          label + "a constrained turn cancels after its first emitted token without requiring complete JSON",
          cancelled.result.error);
    const auto after_cancel = generate(*session, continued);
    check(oracle.prefix(continued, after_cancel) && !after_cancel.result.cancelled,
          label + "a turn after cancellation has fresh grammar and cancel state", after_cancel.result.error);
    const auto ordinary_after_cancel = generate(*session, plain_req);
    check(ordinary_after_cancel.result.ok && reference.result.ok && ordinary_after_cancel.pieces == reference.pieces,
          label + "ordinary generation still matches its reference after cancellation",
          ordinary_after_cancel.result.error);
}

void speculation(const std::string & model, const GrammarOracle & oracle) {
    auto cfg = config(model, false);
    cfg.spec.source = bmoe::DraftSource::ngram;
    cfg.spec.ngram_min_match = 1;
    cfg.spec.ngram_max_match = 3;
    cfg.spec.draft_max = 3;
    std::string error;
    auto session = bmoe::Session::open(cfg, error);
    check(session != nullptr, "the speculative session opens", error);
    if (!session) return;
    auto req =
        request(R"({"const":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"})", 48);
    req.messages = {{"user", "Draft examples: AX AX AX \"X \"X `X `X\nX\nX\tX\tX"}};
    const auto drafted = generate(*session, req);
    const int illegal = oracle.refused_drafts(cfg, req, drafted);
    check(oracle.prefix(req, drafted) && drafted.result.summary.drafted_steps > 0 &&
              drafted.result.summary.mtp_drafted > drafted.result.summary.mtp_accepted && illegal > 0,
          "ngram proposals include illegal tokens that never enter the constrained output",
          drafted.result.error + " drafted=" + std::to_string(drafted.result.summary.mtp_drafted) +
              " accepted=" + std::to_string(drafted.result.summary.mtp_accepted) +
              " illegal-first-rejected=" + std::to_string(illegal));

    req.clear_kv = false;
    const auto after = generate(*session, req);
    check(oracle.prefix(req, after) && after.result.summary.n_prompt == 1 && after.pieces == drafted.pieces,
          "a speculative turn after rejected drafts reuses clean KV and produces the same constrained output",
          after.result.error);
    const auto ordinary = generate(*session, request("", 8));
    check(ordinary.result.ok && !ordinary.result.cancelled,
          "ordinary generation remains usable after constrained speculative turns", ordinary.result.error);
}

void streamed_identity(const std::string & model, bool sampled, const GrammarOracle & oracle) {
    const std::string label = sampled ? "stochastic: " : "greedy: ";
    const auto req = request(R"({"type":"object"})", 16);
    const auto run = [&](const bmoe::SessionConfig & cfg) {
        std::string error;
        auto session = bmoe::Session::open(cfg, error);
        if (session) return generate(*session, req);
        Captured captured;
        captured.result.error = error;
        return captured;
    };
    const auto resident = run(config(model, sampled));
    check(oracle.prefix(req, resident) && !resident.result.cancelled && resident.expert_bytes == 0,
          label + "the constrained resident reference generates without expert reads", resident.result.error);
    for (const int cache_mb : {0, 2}) {
        auto cfg = config(model, sampled);
        cfg.moe.enabled = true;
        cfg.moe.cache_mb = cache_mb;
        cfg.moe.io_threads = 4;
        cfg.moe.force_cache = cache_mb > 0;
        const auto streamed = run(cfg);
        const std::string mode = cache_mb ? "small LRU" : "cache off";
        check(oracle.prefix(req, streamed) && !streamed.result.cancelled && streamed.expert_bytes > 0,
              label + "the constrained streamed path performs expert reads (" + mode + ")", streamed.result.error);
        check(resident.result.ok && streamed.result.ok && resident.pieces == streamed.pieces &&
                  resident.result.generated_text == streamed.result.generated_text,
              label + "constrained resident and streamed output match token-for-token and byte-for-byte (" + mode +
                  ")");
    }
}
} // namespace

int main(int argc, char ** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: bmoe_output_constraint_session_test <tiny-model.gguf>\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bmoe::register_backends();
    const GrammarOracle oracle(argv[1]);
    check(oracle.model != nullptr, "the fixture's vocabulary-only grammar oracle opens");
    if (!oracle.model) return 1;
    lifecycle(argv[1], false, oracle);
    lifecycle(argv[1], true, oracle);
    speculation(argv[1], oracle);
    streamed_identity(argv[1], false, oracle);
    streamed_identity(argv[1], true, oracle);
    return failures ? 1 : 0;
}
