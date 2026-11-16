// The sampling and output fields a request may carry, read once for every protocol: what each
// protocol calls them, the bounds llama.cpp's server holds them to, and which protocol has which.
// JSON in, a ChatRequest's fields out; nothing else, so every rule is tested without a model.
#pragma once

#include "chat_turn.h"
#include "json_util.h"

#include <string>

namespace bmoe::server {

// Which names a body uses. Chat completions reads the most: OpenAI's fields and llama-server's
// (top_k, min_p, repeat_penalty, repeat_last_n, grammar, n_probs). Messages reads Anthropic's
// (temperature, top_p, top_k, stop_sequences), Responses OpenAI's (temperature, top_p).
enum class SamplingDialect { Chat, Messages, Responses };

// The most candidates a token reports beside itself (OpenAI's bound on top_logprobs).
inline constexpr int k_max_top_logprobs = 20;

// Reads the request's sampling, stop text, grammar and logprobs into `turn`. A field left out
// leaves the session's setting; a field of the wrong type or out of its range is a 400 with `err`
// naming it, never a silent clamp. `n` other than 1 is refused: the engine answers one choice.
bool parse_request_sampling(const json & body, SamplingDialect dialect, ChatRequest & turn, std::string & err);

} // namespace bmoe::server
