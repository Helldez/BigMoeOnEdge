#include "text_completion.h"

#include "llama_compat.h"
#include "request_sampling.h"

namespace bmoe::server {

namespace {

bool read_prompt(const json & body, std::string & out, std::string & err) {
    const json p = body.value("prompt", json());
    if (p.is_string()) {
        out = p.get<std::string>();
    } else if (p.is_array() && p.size() == 1 && p[0].is_string()) {
        out = p[0].get<std::string>();
    } else if (p.is_array() && !p.empty() && p[0].is_string()) {
        err = "prompt must be one string: several would be several choices, and this server answers one";
        return false;
    } else if (p.is_array() && !p.empty()) {
        err = "prompt must be text: a prompt of token ids is not read here (/detokenize turns one into text)";
        return false;
    } else {
        err = "prompt must be a non-empty string";
        return false;
    }
    if (out.empty()) {
        err = "prompt must be a non-empty string";
        return false;
    }
    return true;
}

// The fields a client could expect to change the answer and this server does not do: refused, so
// nobody reads an answer made without them as one made with them.
bool refuse_unsupported(const json & body, CompletionDialect dialect, std::string & err) {
    const auto set = [&](const char * k) { return body.contains(k) && !body[k].is_null(); };
    if (set("grammar") || set("json_schema") || set("response_format")) {
        err = "a text completion takes no grammar or response format: use /v1/chat/completions for those";
        return false;
    }
    // Each token's probabilities are a chat answer's field here; a completion would compute them and
    // have nowhere to put them.
    for (const char * k : {"logprobs", "n_probs", "top_logprobs"})
        if (set(k) && body[k] != json(false) && body[k] != json(0)) {
            err = std::string(k) + " is not reported on a text completion: use /v1/chat/completions for it";
            return false;
        }
    if (dialect == CompletionDialect::OpenAi) {
        if (body.contains("echo") && body["echo"] == json(true)) {
            err = "echo is not supported: the answer holds the completion only";
            return false;
        }
        if (set("suffix")) {
            err = "suffix is not supported: this server does not fill in the middle";
            return false;
        }
        if (body.contains("best_of") &&
            !(body["best_of"].is_number_integer() && body["best_of"].get<long long>() == 1)) {
            err = "best_of must be 1: this server answers one choice per request";
            return false;
        }
    }
    return true;
}

json llama_answer(const ChatResponseMeta & meta,
                  const std::string & content,
                  const RunResult & r,
                  const std::string & finish,
                  const std::string & prompt) {
    const json usage = usage_json(r.summary);
    return {{"index", 0},
            {"id_slot", 0},
            {"content", content},
            {"tokens", json::array()},
            {"stop", true},
            {"model", meta.model},
            {"prompt", prompt},
            {"tokens_predicted", r.summary.n_generated},
            {"tokens_evaluated", usage["prompt_tokens"]},
            {"tokens_cached", r.summary.n_past},
            {"truncated", false},
            {"has_new_line", content.find('\n') != std::string::npos},
            {"stop_type", llama_stop_type(finish)},
            {"stopping_word", ""},
            {"timings", timings_json(r.summary)}};
}

} // namespace

bool parse_text_completion(const json & body,
                           CompletionDialect dialect,
                           const RunConfig & cfg,
                           TextCompletionRequest & out,
                           std::string & err) {
    if (!body.is_object()) {
        err = "the request body must be a JSON object";
        return false;
    }
    ChatRequest & turn = out.turn;
    turn.raw = true;
    if (!read_prompt(body, turn.prompt, err) || !refuse_unsupported(body, dialect, err)) return false;

    // The sampling fields and stop text by the chat endpoint's rules. OpenAI's legacy logprobs is a
    // count where the chat endpoint's is a boolean; refuse_unsupported let through only 0 or false.
    json sampling = body;
    sampling.erase("logprobs");
    if (!parse_request_sampling(sampling, SamplingDialect::Chat, turn, err)) return false;

    turn.n_predict = cfg.n_predict;
    turn.think = false; // a raw prompt has no template to ask for reasoning
    bool given = false;
    if (dialect == CompletionDialect::Llama) {
        // llama-server's -1 is "until the context is full": the session's own budget here.
        const bool unbounded = body.contains("n_predict") && body["n_predict"].is_number_integer() &&
                               body["n_predict"].get<long long>() < 0;
        if (!unbounded && !read_token_budget(body, "n_predict", cfg, given, turn.n_predict, err)) return false;
        if (!given && !read_token_budget(body, "max_tokens", cfg, given, turn.n_predict, err)) return false;
    } else if (!read_token_budget(body, "max_tokens", cfg, given, turn.n_predict, err)) {
        return false;
    }

    out.stream = body.contains("stream") && body["stream"].is_boolean() && body["stream"].get<bool>();
    out.include_usage = body.contains("stream_options") && body["stream_options"].is_object() &&
                        body["stream_options"].value("include_usage", json(false)) == json(true);
    return true;
}

std::string llama_stop_type(const std::string & finish) {
    if (finish == "length") return "limit";
    if (finish == "stop") return "eos";
    return "none";
}

json text_completion_json(CompletionDialect dialect,
                          const ChatResponseMeta & meta,
                          const RunResult & r,
                          const std::string & finish,
                          const std::string & prompt) {
    if (dialect == CompletionDialect::Llama) return llama_answer(meta, r.generated_text, r, finish, prompt);
    return {
        {"id", meta.id},
        {"object", "text_completion"},
        {"created", meta.created},
        {"model", meta.model},
        {"choices",
         json::array({{{"index", 0}, {"text", r.generated_text}, {"logprobs", nullptr}, {"finish_reason", finish}}})},
        {"usage", usage_json(r.summary)},
        {"timings", timings_json(r.summary)}};
}

json text_completion_chunk(CompletionDialect dialect, const ChatResponseMeta & meta, const std::string & text) {
    if (dialect == CompletionDialect::Llama)
        return {{"index", 0}, {"id_slot", 0}, {"content", text}, {"tokens", json::array()}, {"stop", false}};
    return {
        {"id", meta.id},
        {"object", "text_completion"},
        {"created", meta.created},
        {"model", meta.model},
        {"choices", json::array({{{"index", 0}, {"text", text}, {"logprobs", nullptr}, {"finish_reason", nullptr}}})}};
}

json text_completion_last_chunk(CompletionDialect dialect,
                                const ChatResponseMeta & meta,
                                const RunResult & r,
                                const std::string & finish,
                                const std::string & prompt,
                                bool usage) {
    // llama-server's last object carries no text: the pieces before it held all of it.
    if (dialect == CompletionDialect::Llama) return llama_answer(meta, "", r, finish, prompt);
    json c = text_completion_chunk(dialect, meta, "");
    c["choices"][0]["finish_reason"] = finish;
    if (usage) c["usage"] = usage_json(r.summary);
    c["timings"] = timings_json(r.summary);
    return c;
}

} // namespace bmoe::server
