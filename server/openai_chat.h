// The OpenAI chat-completions protocol at the server's edge: a request body into one engine turn,
// and a finished turn into the response objects. JSON in, JSON out, nothing else - no session, no
// HTTP - so every rule here (which roles and fields are read, what a tool call looks like on the
// wire, how tokens are counted) is tested without a model.
//
// What the protocol MEANS for a model - how its template writes a tool, how a call is read back out
// of the text - is not here either: that is llama.cpp's chat layer, reached through the session.
#pragma once

#include "bmoe/config.h"
#include "bmoe/runtime.h"
#include "chat_turn.h"
#include "json_util.h"

#include <string>

namespace bmoe::server {

struct OpenAiChatRequest {
    ChatRequest turn;
    bool stream = false;
    bool include_usage = false; // stream_options.include_usage: a last chunk carrying the token counts
};

// Parse a /v1/chat/completions body. `cfg` supplies the defaults a request may leave out
// (n-predict, think). False with `err` set for a body the engine cannot answer.
bool parse_chat_request(const json & body, const RunConfig & cfg, OpenAiChatRequest & out, std::string & err);

// What stays the same across every object of one response.
struct ChatResponseMeta {
    std::string id;
    long long created = 0;
    std::string model;
};

// The model writes a call's name and arguments; the protocol also wants an id the client can answer
// to. Calls that came back without one get an id derived from the response's own.
void ensure_tool_call_ids(std::vector<ChatToolCall> & calls, const std::string & response_id);

// stop | length | tool_calls | cancelled.
std::string finish_reason(const RunResult & r, int n_predict);

// prompt_tokens is the whole prompt the answer was conditioned on; the part the KV already held is
// reported as cached, the way the protocol reports a prompt cache.
json usage_json(const RunSummary & s);

// The non-streaming `chat.completion` object.
json completion_json(const ChatResponseMeta & meta, const RunResult & r, const std::string & finish);

// One streaming `chat.completion.chunk`. `finish` is null until the last choice chunk.
json chunk_json(const ChatResponseMeta & meta, const json & delta, const json & finish);

// The delta that delivers a turn's tool calls, each whole in one piece.
json tool_calls_delta(const std::vector<ChatToolCall> & calls);

// The chunk stream_options.include_usage asks for: no choices, only the counts.
json usage_chunk_json(const ChatResponseMeta & meta, const RunSummary & s);

} // namespace bmoe::server
