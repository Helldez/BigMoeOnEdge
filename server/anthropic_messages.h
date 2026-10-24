// The Anthropic Messages API at the server's edge (POST /v1/messages, /v1/messages/count_tokens):
// what Claude Code speaks. A request becomes the same engine turn a chat request does, and the turn
// becomes a message, or the event stream of one. JSON in, JSON and frames out, no session, no HTTP,
// so every rule here is tested without a model (tests/server_test.cpp).
#pragma once

#include "agent_protocol.h"
#include "bmoe/config.h"
#include "bmoe/runtime.h"
#include "chat_turn.h"
#include "json_util.h"
#include "openai_chat.h"

#include <string>

namespace bmoe::server {

struct AnthropicRequest {
    ChatRequest turn;
    bool stream = false;
};

// Parse a /v1/messages body (count_tokens takes the same one). `cfg` supplies what a request leaves
// out: the budget when max_tokens is absent, thinking when `thinking` is. False with `err` set for a
// body the engine cannot answer: an image or a document, a server tool, a conversation that ends on
// the assistant (a prefilled answer, which the engine does not continue).
bool parse_messages_request(const json & body, const RunConfig & cfg, AnthropicRequest & out, std::string & err);

// The protocol's error object: {"type":"error","error":{"type", "message"}}, its type read from the
// HTTP status. A full context is said the way the protocol says it ("prompt is too long"), which is
// what makes Claude Code compact the conversation instead of failing.
json anthropic_error_json(int status, const std::string & message, const std::string & code);

// end_turn | max_tokens | tool_use, from finish_reason()'s word.
std::string anthropic_stop_reason(const std::string & finish);

// input_tokens is the part of the prompt prefilled for this turn, cache_read_input_tokens the part
// the KV already held: the protocol counts a prompt cache that way, and a client adds them up.
json anthropic_usage_json(const RunSummary & s);

// The non-streaming message object.
json message_json(const ChatResponseMeta & meta, const RunResult & r, const std::string & finish);

// One turn's event stream, frame by frame. start() opens it (message_start), on_token() follows the
// per-token answer, finish() closes it with the final answer, the tool calls, the stop reason and
// the usage; error() ends it on a failed turn instead. Every call returns the frames to write.
class AnthropicStream {
public:
    explicit AnthropicStream(ChatResponseMeta meta) : meta_(std::move(meta)) {}
    std::string start() const;
    static std::string ping();
    std::string on_token(const std::string & text, const std::string & reasoning);
    // `r`'s tool calls must carry their ids (ensure_tool_call_ids).
    std::string finish(const RunResult & r, const std::string & finish);
    static std::string error(int status, const std::string & message, const std::string & code);

private:
    std::string frames(const std::vector<BlockEvent> & events);

    ChatResponseMeta meta_;
    BlockStream blocks_;
    int index_ = 0; // the next content block's index
};

} // namespace bmoe::server
