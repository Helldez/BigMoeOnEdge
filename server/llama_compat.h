// llama-server's own surface, served by this server: the fields and endpoints its clients read
// beside the OpenAI and Anthropic protocols (timings, prompt progress, /health, /props, /metrics,
// /slots, /tokenize, /detokenize, /apply-template). The shapes are llama.cpp's
// (tools/server/server-task.cpp, server-common.cpp), copied as a wire format and not linked: a
// client written for llama-server reads them the same here. JSON and text in, JSON and text out,
// nothing else - no session, no HTTP - so every shape is tested without a model.
#pragma once

#include "bmoe/metrics.h"
#include "bmoe/session.h"
#include "json_util.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bmoe::server {

// The `timings` object llama-server adds to an answer: the prompt part decoded this turn and what
// the context already held (cache_n), and the answer's tokens, each with its time and rate.
json timings_json(const RunSummary & s);

// The `prompt_progress` object of a streamed chunk while the prompt goes in.
json prompt_progress_json(const PrefillProgress & p);

// What every answered turn adds to /metrics. The totals only grow; the window is what a scrape
// averages its rates over, and is emptied by the scrape that reads it, as llama-server's are.
struct ServeTotals {
    long long prompt_tokens = 0; // decoded, the cached part excluded
    long long prompt_cached = 0;
    double prompt_seconds = 0.0;
    long long predicted_tokens = 0;
    double predicted_seconds = 0.0;
    long long turns = 0;
    int n_tokens_max = 0; // the longest context a turn left (prompt + answer)
    // Since the last scrape.
    long long window_prompt_tokens = 0, window_predicted_tokens = 0;
    double window_prompt_seconds = 0.0, window_predicted_seconds = 0.0;

    void add(const RunSummary & s);
};

// /metrics in Prometheus' text format, under llama-server's names (llamacpp:*). `processing` is the
// turns running now and `deferred` those waiting their place.
std::string metrics_text(const ServeTotals & t, int processing, int deferred);

// Fields of /props a client reads: the context, the default answer budget and sampling, the model
// and what it reads, the build. `model_name` and `model_path` are empty when nothing is loaded.
struct PropsFacts {
    int n_ctx = 0;
    int n_predict = 0;
    float temperature = 0.0f;
    int top_k = 0;
    float top_p = 0.0f;
    uint32_t seed = 0;
    float presence_penalty = 0.0f, frequency_penalty = 0.0f, repeat_penalty = 1.0f;
    int repeat_last_n = 64;
    std::string model_name, model_path;
    bool vision = false;
    std::string build;
};
json props_json(const PropsFacts & f);

// /slots: the one slot this server has (it answers one turn at a time).
json slots_json(int n_ctx, bool processing);

// The bodies of /tokenize and /detokenize. False with `err` for a body llama-server would refuse,
// or one it reads and this server does not (content as a mixed array of text and token ids).
struct TokenizeRequest {
    std::string content;
    bool add_special = false;
    bool parse_special = true;
    bool with_pieces = false;
};
bool parse_tokenize_request(const json & body, TokenizeRequest & out, std::string & err);
bool parse_detokenize_request(const json & body, std::vector<int32_t> & tokens, std::string & err);

// A token's piece as /tokenize spells it with with_pieces: the text when it is whole UTF-8, else
// its bytes, as llama-server does for a piece that splits a code point.
json token_piece_json(const std::string & piece);

} // namespace bmoe::server
