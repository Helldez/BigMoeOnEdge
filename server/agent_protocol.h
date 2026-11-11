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

#include <functional>
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
// diverged() says so. Tool calls are not text: they travel as blocks of their own (ToolCallStream),
// and a protocol that cannot interleave closes the open text block before a call's block opens.
class BlockStream {
public:
    std::vector<BlockEvent> update(const std::string & text, const std::string & reasoning);
    std::vector<BlockEvent> finish(const std::string & text, const std::string & reasoning);
    // Closes the open block, if any; text that follows opens a new one.
    std::vector<BlockEvent> close();
    bool diverged() const { return diverged_; }

private:
    void append(BlockKind kind, const std::string & snapshot, bool final, std::vector<BlockEvent> & out);

    std::string sent_[2]; // per kind: everything sent, over every block of that kind
    bool open_ = false;
    BlockKind current_ = BlockKind::Reasoning;
    bool diverged_ = false;
};

struct ToolCallEvent {
    enum class Type { Open, Delta, Close };
    Type type;
    size_t index;          // the call's place in the answer, from 0
    std::string id;        // Open: the id the call keeps to the end
    std::string name;      // Open
    std::string arguments; // Delta: what this event appends to the call's arguments
};

// The calls an answer holds, as the per-token parse sees them (TokenMetrics::tool_calls), turned
// into append-only events every protocol can frame: a call opens, its arguments arrive in pieces,
// and it closes. The rules are BlockStream's, so the same guarantee holds - what a client was sent
// is never contradicted:
//  - a call opens once its arguments have begun: every template writes the name before them, so the
//    name is whole by then, and the id the call keeps is fixed at that moment (the model's own, or
//    `id_for(index)` when it wrote none);
//  - a snapshot whose arguments extend what was sent sends the difference, held back to the last
//    whole UTF-8 character; one that does not sends nothing until a later one extends it again;
//  - calls open in order, and a call opening closes the one before it, whose arguments are complete.
// finish() takes the turn's final calls: it sends what each adds, opens and fills whole any call no
// snapshot showed, closes them all, and gives every call the id its stream announced, so the final
// answer and the stream name the same call the same way. A final call whose arguments do not extend
// what was sent leaves the client with what it holds, and diverged() says so.
class ToolCallStream {
public:
    explicit ToolCallStream(std::function<std::string(size_t)> id_for);
    std::vector<ToolCallEvent> update(const std::vector<ChatToolCall> & calls);
    std::vector<ToolCallEvent> finish(std::vector<ChatToolCall> & calls);
    size_t opened() const { return sent_.size(); }
    bool diverged() const { return diverged_; }

private:
    struct Sent {
        std::string id;
        std::string arguments;
        bool closed = false;
    };
    void advance(size_t i, const ChatToolCall & c, bool final, std::vector<ToolCallEvent> & out);

    std::function<std::string(size_t)> id_for_;
    std::vector<Sent> sent_;
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
