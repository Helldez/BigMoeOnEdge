// Plain text completion: a prompt continued as written, with no chat template, in the two shapes its
// clients send and read - llama-server's own /completion and OpenAI's legacy /v1/completions. The
// shapes are llama.cpp's (tools/server/server-task.cpp) and OpenAI's, copied as a wire format and not
// linked. JSON in, JSON out, nothing else - no session, no HTTP - so every rule is tested without a model.
//
// The turn is the chat endpoint's turn with ChatRequest::raw set: the prompt is fed from an empty
// context (GenerateRequest::raw), so a client that alternates completions and chats pays a prefill.
#pragma once

#include "bmoe/config.h"
#include "bmoe/runtime.h"
#include "chat_turn.h"
#include "json_util.h"
#include "openai_chat.h"

#include <string>

namespace bmoe::server {

// Which of the two shapes a body and its answer take.
enum class CompletionDialect {
    Llama,  // /completion: prompt, n_predict; answers {content, stop, timings, ...}
    OpenAi, // /v1/completions: prompt, max_tokens; answers a text_completion object
};

struct TextCompletionRequest {
    ChatRequest turn; // raw, with the prompt and the request's sampling
    bool stream = false;
    bool include_usage = false; // OpenAI's stream_options.include_usage
};

// Parse a completion body. `prompt` is a string, or an array holding one string (several would be
// several choices, which this server does not answer). The sampling fields are the chat endpoint's
// (request_sampling.h); a grammar, logprobs, a token-id prompt and `echo`/`suffix` are refused with
// `err` rather than ignored. `cfg` supplies the default answer budget.
bool parse_text_completion(const json & body,
                           CompletionDialect dialect,
                           const RunConfig & cfg,
                           TextCompletionRequest & out,
                           std::string & err);

// llama-server's stop_type: "eos" (the model ended, or a stop text), "limit" (the budget), "none".
std::string llama_stop_type(const std::string & finish);

// The whole answer. `prompt` is echoed by llama-server's shape, as its clients expect.
json text_completion_json(CompletionDialect dialect,
                          const ChatResponseMeta & meta,
                          const RunResult & r,
                          const std::string & finish,
                          const std::string & prompt);

// One streamed piece of text. llama-server's carries stop:false, OpenAI's a null finish_reason.
json text_completion_chunk(CompletionDialect dialect, const ChatResponseMeta & meta, const std::string & text);

// The streamed answer's last object: llama-server's carries stop:true with the counts and timings,
// OpenAI's an empty text with the finish_reason, and the usage when `usage` (include_usage) asks.
json text_completion_last_chunk(CompletionDialect dialect,
                                const ChatResponseMeta & meta,
                                const RunResult & r,
                                const std::string & finish,
                                const std::string & prompt,
                                bool usage);

} // namespace bmoe::server
