// A conversation sent to an OpenAI-compatible chat service (a saved provider, docs/agent.md), and
// its streamed answer read back. JSON in, JSON out: the request body a ChatRequest becomes, and the
// server-sent events of the answer folded into the text, reasoning and tool calls an engine turn
// would have given. No HTTP here; the transfer is http_fetch.h's.
#pragma once

#include "chat_turn.h"
#include "json_util.h"

#include <string>
#include <vector>

namespace bmoe::server {

// The /chat/completions body for `req` on `model`: messages with their tool calls and answers,
// tools as functions, the answer budget, and stream on.
json provider_request_json(const ChatRequest & req, const std::string & model, bool stream);

// Folds the `data:` events of a streamed answer, fed in pieces of any size.
class ProviderStream {
public:
    void feed(const char * data, size_t n);
    // The body was not a stream: a 4xx or 5xx error object, or a whole answer from a service that
    // ignored `stream`. Called with everything that was fed, once the transfer ended.
    void finish_body(const std::string & whole);

    const std::string & text() const { return text_; }
    const std::string & reasoning() const { return reasoning_; }
    std::vector<ChatToolCall> tool_calls() const;
    const std::string & finish_reason() const { return finish_; }
    bool done() const { return done_; }
    // What the service said went wrong, from an `error` object in the stream or the body.
    const std::string & error() const { return error_; }
    bool saw_event() const { return events_ > 0; }
    // Whether the last feed() changed the text or the reasoning: a token for the UI.
    bool changed() {
        const bool c = changed_;
        changed_ = false;
        return c;
    }

private:
    void line(const std::string & l);
    void chunk(const json & j);

    std::string pending_;
    std::string text_, reasoning_, finish_, error_;
    struct Call {
        std::string id, name, arguments;
    };
    std::vector<Call> calls_; // by the index the stream gives each call
    bool done_ = false;
    bool changed_ = false;
    int events_ = 0;
};

// The message an error body carries (`{"error": {"message"}}`, `{"error": "..."}`, `{"message"}`),
// or the body itself, cut short, when it has none of those.
std::string provider_error_message(const std::string & body);

} // namespace bmoe::server
