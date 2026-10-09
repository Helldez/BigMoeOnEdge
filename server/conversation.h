// How an OpenAI-style message list becomes one engine turn.
//
// The engine keeps a conversation in its KV cache and renders one user turn per generation; an
// OpenAI client resends the whole history on every request. This is the rule that joins the two,
// kept pure (no session, no HTTP) so it is tested on its own: continue the KV only when the request
// is EXACTLY the conversation the KV holds plus one user message, and otherwise start fresh and say
// what could not be replayed. Comparing what the client sent with what the KV holds is the only safe
// test — an edited turn, a regenerated answer or another client's chat all fail it.
#pragma once

#include <string>
#include <vector>

namespace bmoe::server {

struct ChatMessage {
    std::string role; // system | user | assistant
    std::string content;
    bool operator==(const ChatMessage & o) const { return role == o.role && content == o.content; }
};

struct TurnPlan {
    bool ok = true;
    std::string error;
    std::string prompt;           // what the engine is fed
    bool clear_kv = true;         // false: continue the KV
    bool history_dropped = false; // earlier turns the fresh KV does not hold
};

// `history` is the conversation the KV holds (empty after a reset), `messages` the request's.
TurnPlan plan_turn(const std::vector<ChatMessage> & history, const std::vector<ChatMessage> & messages);

} // namespace bmoe::server
