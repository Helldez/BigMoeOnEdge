// The OpenAI Responses API at the server's edge (POST /v1/responses): what Codex CLI speaks. A
// request becomes the same engine turn a chat request does, and the turn becomes a response object,
// or the event stream of one. JSON in, JSON and frames out, no session, no HTTP, so every rule here
// is tested without a model (tests/server_test.cpp).
//
// The server keeps no responses: every request carries its whole conversation in `input`, as Codex
// sends it with store=false. previous_response_id, a stored item reference and a background
// response are refused rather than answered without the state they name.
#pragma once

#include "agent_protocol.h"
#include "bmoe/config.h"
#include "bmoe/runtime.h"
#include "chat_turn.h"
#include "json_util.h"
#include "openai_chat.h"

#include <string>

namespace bmoe::server {

struct ResponsesRequest {
    ChatRequest turn;
    bool stream = false;
};

// Parse a /v1/responses body. `cfg` supplies what a request leaves out (the budget, thinking). A
// built-in tool (web search, a namespace of tools, a freeform tool) is dropped: the server runs none,
// and Codex sends some whether or not the provider has them; a function tool is described to the
// model. False with `err` set for a body the engine cannot answer.
bool parse_responses_request(const json & body, const RunConfig & cfg, ResponsesRequest & out, std::string & err);

// The protocol's error object, {"error": {"message", "type", "param", "code"}}. A full context has
// the code a client knows it by, context_length_exceeded; every other code is this server's own.
json responses_error_json(int status, const std::string & message, const std::string & code);

// The response object of a finished turn. `r`'s tool calls must carry their ids.
json response_json(const ChatResponseMeta & meta, const RunResult & r, const std::string & finish);

// One turn's event stream, frame by frame, as AnthropicStream: start() sends response.created and
// response.in_progress, on_token() follows the per-token answer, finish() closes every item and ends
// with response.completed (or response.incomplete on the budget), error() with response.failed.
class ResponsesStream {
public:
    explicit ResponsesStream(ChatResponseMeta meta) : meta_(std::move(meta)) {}
    std::string start();
    std::string on_token(const std::string & text, const std::string & reasoning);
    std::string finish(const RunResult & r, const std::string & finish);
    std::string error(int status, const std::string & message, const std::string & code);

private:
    std::string event(const std::string & type, json data);
    std::string frames(const std::vector<BlockEvent> & events);

    ChatResponseMeta meta_;
    BlockStream blocks_;
    int seq_ = 0;                 // sequence_number, on every event
    json output_ = json::array(); // the items closed so far, in order: what the final response lists
    std::string open_;            // the open item's text
    std::string open_id_;
};

} // namespace bmoe::server
