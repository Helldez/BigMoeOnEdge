// What the two block-structured agent protocols share: the Anthropic Messages API (Claude Code) and
// the OpenAI Responses API (Codex CLI). Both build the ChatRequest the chat endpoint builds and read
// the same ChatOutcome; what they add is a stream that cannot take anything back.
//
// The chat endpoint streams deltas and, when the chat parser reclassifies text it already sent,
// sends the whole state again with `reset`. These protocols have no such message: a block is opened,
// appended to and closed, and what a client was sent is what it keeps (and sends back in the next
// request's history). BlockStream is the rule that keeps that true. JSON in, frames out, no HTTP.
#pragma once

#include "chat_turn.h"
#include "json_util.h"

#include <string>
#include <vector>

namespace bmoe::server {

enum class BlockKind { Reasoning, Text };

struct BlockEvent {
    enum class Type { Open, Delta, Close };
    Type type;
    BlockKind kind;
    std::string text; // Delta only: what this event appends to its block
};

// The answer and its reasoning, as the per-token parse sees them (TokenMetrics::text, ::reasoning),
// turned into append-only blocks. A snapshot that extends what was sent sends the difference, held
// back to the last whole UTF-8 character; one that does not (the parser has reclassified text it
// showed before) sends nothing for that channel until a later snapshot extends it again, so nothing
// sent is ever contradicted. finish() sends what the final answer adds and closes the open block.
//
// The parser holds back markup it cannot classify yet, so in practice every snapshot extends the
// last; when a final answer does not extend what was sent, the client keeps the text it holds and
// diverged() says so. Tool calls are not text: they are known whole once the turn is parsed, and
// travel as blocks of their own after this stream finishes.
class BlockStream {
public:
    std::vector<BlockEvent> update(const std::string & text, const std::string & reasoning);
    std::vector<BlockEvent> finish(const std::string & text, const std::string & reasoning);
    bool diverged() const { return diverged_; }

private:
    void append(BlockKind kind, const std::string & snapshot, bool final, std::vector<BlockEvent> & out);

    std::string sent_[2]; // per kind: everything sent, over every block of that kind
    bool open_ = false;
    BlockKind current_ = BlockKind::Reasoning;
    bool diverged_ = false;
};

// One server-sent event with a name, as both protocols frame them.
std::string sse_event(const std::string & name, const json & data);

// A chat template takes one system message, at the start (most raise on any other). Agents send
// more: Claude Code adds system turns mid-conversation, Codex sends several developer messages.
// The leading ones are joined into one system message; a later one is passed as a user message,
// which keeps its text where it was, and keeps the prompt before it unchanged, so the context the
// KV already holds is reused.
void place_system_messages(std::vector<ChatMessage> & messages);

// The tokens a request's prompt is estimated to take, from its length: the host offers no tokenizer
// outside a turn. What count_tokens answers.
int estimate_prompt_tokens(const ChatRequest & req);

} // namespace bmoe::server
