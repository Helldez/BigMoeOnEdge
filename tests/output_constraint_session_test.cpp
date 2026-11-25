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
#include <cctype>
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

// A caller-owned stop is independent of the session latch and ends with the request, even
// before a token exists. Exercise real graph callbacks, then another API on the same context.
void caller_stop_lifecycle(const std::string & model, bool sampled) {
    const std::string label = sampled ? "caller stop, stochastic: " : "caller stop, greedy: ";
    auto cfg = config(model, sampled);
    cfg.decide.enabled = true;
    std::string error;
    auto session = bmoe::Session::open(cfg, error);
    check(session != nullptr, label + "the real session opens", error);
    if (!session) return;
    const auto ordinary = request("", 8);
    const auto reference = generate(*session, ordinary);
    check(reference.result.ok, label + "the ordinary reference completes", reference.result.error);

    std::atomic<bool> stop{true};
    auto req = request(R"({"type":"object"})", 16);
    req.stop = &stop;
    const auto before_token = generate(*session, req);
    check(before_token.result.ok && before_token.result.cancelled && before_token.rows.empty() && stop.load(),
          label + "a pre-existing caller flag survives the session latch reset and cancels before any token",
          before_token.result.error);

    bmoe::DecideRequest decision;
    decision.prefix = "Choose a label.";
    decision.choices = {"A", "B"};
    const auto decided = session->decide(decision);
    check(decided.ok && !decided.cancelled && stop.load(),
          label + "a later decision ignores the retired request's still-raised flag", decided.error);

    stop = false;
    int emitted = 0;
    const auto during_decode = session->generate(req, [&](const bmoe::TokenMetrics &) {
        ++emitted;
        stop = true;
    });
    check(during_decode.ok && during_decode.cancelled && emitted == 1 && stop.load(),
          label + "a caller flag raised at the first token stops a constrained turn", during_decode.error);
    const auto recovered = generate(*session, ordinary);
    check(recovered.result.ok && !recovered.result.cancelled && recovered.pieces == reference.pieces && stop.load(),
          label + "the next ordinary request ignores that flag and matches its reference", recovered.result.error);
}

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

    // A request that samples is decoded without a draft and with its own sampling, as a session that
    // never drafts would decode it; the conversation goes on without one until it is cleared.
    auto plain_session = bmoe::Session::open(config(model, false), error);
    check(plain_session != nullptr, "the session without a draft opens", error);
    if (!plain_session) return;
    auto sampled = request("", 16);
    sampled.sampling = bmoe::RequestSampling{};
    sampled.sampling->temp = 1.0f;
    sampled.sampling->seed = 1234u;
    const auto drawn = generate(*session, sampled);
    const auto drawn_plain = generate(*plain_session, sampled);
    check(drawn.result.ok && drawn.result.summary.drafted_steps == 0 && drawn_plain.result.ok &&
              drawn.result.generated_text == drawn_plain.result.generated_text,
          "a sampling request on a drafting session decodes without a draft, its sampling applied",
          drawn.result.error + " drafted_steps=" + std::to_string(drawn.result.summary.drafted_steps));
    auto followed = req;
    followed.clear_kv = false;
    const auto follow = generate(*session, followed);
    check(follow.result.ok && follow.result.summary.drafted_steps == 0,
          "the conversation that sampled goes on without a draft", follow.result.error);
    auto renewed = req;
    renewed.clear_kv = true;
    const auto fresh = generate(*session, renewed);
    check(fresh.result.ok && fresh.result.summary.drafted_steps > 0 && fresh.pieces == drafted.pieces,
          "a new conversation drafts again, as before", fresh.result.error);
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

// A request's own sampling, stop text, grammar, logprobs and prefill progress, on real decodes: each
// changes what the request asked and nothing else, and a request that asks nothing decodes as before.
void request_controls(const std::string & model) {
    std::string error;
    auto greedy = bmoe::Session::open(config(model, false), error);
    auto sampled = bmoe::Session::open(config(model, true), error);
    check(greedy && sampled, "request controls: both sessions open", error);
    if (!greedy || !sampled) return;

    const auto plain = request("", 24);
    const auto reference = generate(*greedy, plain);
    check(reference.result.ok && reference.result.generated_text.size() >= 8, "request controls: a greedy reference",
          reference.result.error);
    if (!reference.result.ok || reference.result.generated_text.size() < 8) return;
    const std::string & ref = reference.result.generated_text;

    // temperature 0 on a session that samples is the greedy answer.
    auto req = plain;
    req.sampling = bmoe::RequestSampling{};
    req.sampling->temp = 0.0f;
    check(generate(*sampled, req).result.generated_text == ref,
          "request controls: temperature 0 decodes greedily on a sampling session");

    // A fixed seed draws the same answer twice; the session's own seed is left alone.
    req.sampling->temp = 1.0f;
    req.sampling->seed = 1234u;
    const auto first = generate(*greedy, req), second = generate(*greedy, req);
    check(first.result.ok && first.result.generated_text == second.result.generated_text,
          "request controls: a request's seed reproduces its answer");
    check(generate(*greedy, plain).result.generated_text == ref,
          "request controls: the next request without sampling decodes as before");

    // A stop ends the answer before it, says which stop, and is never shown while it forms. Two
    // ASCII characters past the first: the fixture writes stray bytes too, which the answer's text
    // shows as replacement characters, so a stop cut from that text may not be in what was generated.
    size_t at = 1;
    while (at + 1 < ref.size() && !(std::isalnum((unsigned char) ref[at]) && std::isalnum((unsigned char) ref[at + 1])))
        ++at;
    check(at + 1 < ref.size(), "request controls: the reference has two ASCII characters to stop at");
    if (at + 1 >= ref.size()) return;
    const std::string stop = ref.substr(at, 2);
    auto stopped = plain;
    stopped.stop_text = {"\x01never", stop};
    const auto s = generate(*greedy, stopped);
    const std::string want = ref.substr(0, ref.find(stop));
    bool shown_clean = true;
    for (const auto & row : s.rows)
        if (row.text.find(stop) != std::string::npos || want.compare(0, row.text.size(), row.text) != 0)
            shown_clean = false;
    check(s.result.ok && s.result.generated_text == want && s.result.stop_text == stop && shown_clean,
          "request controls: a stop ends the answer before it and is never streamed", s.result.generated_text);

    // A GBNF grammar holds the whole answer.
    auto gram = plain;
    gram.grammar = R"(root ::= "ab" "c"*)";
    gram.n_predict = 6;
    const auto g = generate(*greedy, gram);
    const std::string & gt = g.result.generated_text;
    check(g.result.ok && gt.rfind("ab", 0) == 0 && gt.find_first_not_of('c', 2) == std::string::npos,
          "request controls: a request's grammar holds the answer", gt + " " + g.result.error);
    gram.grammar = "root ::= (";
    const auto bad = generate(*greedy, gram);
    check(!bad.result.ok && bad.result.error_kind == bmoe::RunError::InvalidRequest,
          "request controls: a grammar that does not parse is the request's fault");

    // logprobs: every token reports its candidates, most likely first, and greedy takes the first.
    auto probs = plain;
    probs.n_probs = 3;
    probs.n_predict = 6;
    const auto p = generate(*greedy, probs);
    bool sane = !p.rows.empty();
    for (const auto & row : p.rows) {
        sane = sane && row.has_logprobs && row.top_logprobs.size() == 3 && row.logprob <= 0.0f &&
               row.top_logprobs[0].logprob >= row.top_logprobs[1].logprob &&
               row.top_logprobs[1].logprob >= row.top_logprobs[2].logprob &&
               row.top_logprobs[0].logprob == row.logprob && row.top_logprobs[0].piece == row.piece;
    }
    check(sane && p.result.generated_text == ref.substr(0, p.result.generated_text.size()),
          "request controls: logprobs are reported and change nothing");

    // Prefill progress: from nothing decoded to the whole prompt but its cached part.
    std::vector<bmoe::PrefillProgress> steps;
    auto prog = plain;
    prog.n_predict = 2;
    prog.on_prefill = [&](const bmoe::PrefillProgress & pp) { steps.push_back(pp); };
    generate(*greedy, prog);
    check(steps.size() >= 2 && steps.front().processed == 0 &&
              steps.back().processed == steps.back().total - steps.back().cached && steps.back().total > 0,
          "request controls: prefill progress runs from 0 to the prompt's uncached part");

    // The tokenizer and the template outside a turn.
    std::string text, prompt, err;
    const auto ids = greedy->tokenize("hello", false, false);
    check(!ids.empty() && greedy->detokenize(ids, text) && text == "hello" && !greedy->detokenize({-1}, text) &&
              greedy->token_piece(ids[0]) == text.substr(0, greedy->token_piece(ids[0]).size()),
          "request controls: tokenize and detokenize round-trip, an id outside the vocabulary refused");
    bmoe::GenerateRequest chat;
    chat.messages = {{"user", "hi"}};
    check(greedy->render_chat(chat, prompt, err) && prompt.find("hi") != std::string::npos,
          "request controls: the template renders a conversation outside a turn", err);
}

// The penalty parameters (#715) are the defaults of a request's own penalties, on real decodes: a
// request that sets none samples as if it had sent the session's, one that sets its own wins, and in a
// chat (the server's way, the KV kept) every turn gets the same as a request that names it each time.
void session_penalties(const std::string & model) {
    std::string error;
    auto greedy = bmoe::Session::open(config(model, false), error);
    auto sampled = bmoe::Session::open(config(model, true), error);
    auto cfg = config(model, false);
    cfg.sampling.presence_penalty = 1.5f;
    auto penalized = bmoe::Session::open(cfg, error);
    cfg = config(model, true);
    cfg.sampling.presence_penalty = 1.5f;
    auto sampled_penalized = bmoe::Session::open(cfg, error);
    check(greedy && sampled && penalized && sampled_penalized, "penalties: the sessions open", error);
    if (!greedy || !sampled || !penalized || !sampled_penalized) return;

    const auto plain = request("", 24);
    const std::string ref = generate(*greedy, plain).result.generated_text;
    auto asked = plain;
    asked.sampling = bmoe::RequestSampling{};
    asked.sampling->presence_penalty = 1.5f;
    const std::string want = generate(*greedy, asked).result.generated_text;
    // The fixture repeats itself greedily; without that the checks below could not tell anything apart.
    check(!ref.empty() && !want.empty() && want != ref, "penalties: a presence penalty changes the greedy answer",
          ref + " | " + want);

    const auto got = generate(*penalized, plain);
    check(got.result.ok && got.result.generated_text == want,
          "penalties: a request that sets no sampling gets the session's penalty", got.result.generated_text);
    auto temp_only = plain;
    temp_only.sampling = bmoe::RequestSampling{};
    temp_only.sampling->temp = 0.0f;
    check(generate(*penalized, temp_only).result.generated_text == want,
          "penalties: a request that sets only a temperature keeps the session's penalty");
    auto off = plain;
    off.sampling = bmoe::RequestSampling{};
    off.sampling->presence_penalty = 0.0f;
    check(generate(*penalized, off).result.generated_text == ref, "penalties: a request's own penalty wins");
    check(generate(*greedy, plain).result.generated_text == ref, "penalties: a session without them decodes as before");

    // Sampling too: the session's temperature and seed, with its penalty, draw as the request would.
    const auto drawn = generate(*sampled_penalized, plain), drawn_again = generate(*sampled_penalized, plain);
    const auto drawn_asked = generate(*sampled, asked);
    check(drawn.result.ok && drawn.result.generated_text == drawn_asked.result.generated_text &&
              drawn_again.result.generated_text == drawn.result.generated_text,
          "penalties: with a temperature, the session's penalty draws what the request's does, turn after turn");

    // A chat: the second turn continues the KV, as the server's turns do.
    auto first = plain, first_asked = asked;
    auto second = plain, second_asked = asked;
    second.prompt = second_asked.prompt = "y";
    second.clear_kv = second_asked.clear_kv = false;
    const auto p1 = generate(*penalized, first), p2 = generate(*penalized, second);
    const auto a1 = generate(*greedy, first_asked), a2 = generate(*greedy, second_asked);
    check(p1.result.ok && p2.result.ok && p1.result.generated_text == a1.result.generated_text &&
              p2.result.generated_text == a2.result.generated_text,
          "penalties: in a chat, every turn gets the session's penalty as if the request named it",
          p2.result.generated_text + " | " + a2.result.generated_text);
}

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
    caller_stop_lifecycle(argv[1], false);
    caller_stop_lifecycle(argv[1], true);
    lifecycle(argv[1], false, oracle);
    lifecycle(argv[1], true, oracle);
    speculation(argv[1], oracle);
    streamed_identity(argv[1], false, oracle);
    streamed_identity(argv[1], true, oracle);
    request_controls(argv[1]);
    session_penalties(argv[1]);
    return failures ? 1 : 0;
}
