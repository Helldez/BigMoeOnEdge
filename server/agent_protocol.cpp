#include "agent_protocol.h"

namespace bmoe::server {

namespace {

// Bytes of request text per prompt token, for an estimate. Below a tokenizer's usual average on
// English prose (about 4) on purpose: an agent's prompt is mostly JSON schemas and code, which
// split finer, and an estimate that runs high makes a client compact early rather than overflow
// the context. Not yet checked against a real tokenizer's count on an agent's prompt.
constexpr size_t kEstimateBytesPerToken = 3;
// What a template adds around each message (its role header and end marker), in bytes of text.
constexpr size_t kEstimateMessageOverheadBytes = 16;

bool starts_with(const std::string & s, const std::string & prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

void BlockStream::append(BlockKind kind, const std::string & snapshot, bool final, std::vector<BlockEvent> & out) {
    std::string & sent = sent_[(int) kind];
    if (!starts_with(snapshot, sent)) {
        diverged_ = diverged_ || final;
        return;
    }
    std::string delta = snapshot.substr(sent.size());
    // A token may end inside a code point. Held until the next one completes it: a frame is JSON,
    // and half a character there would be replaced, in a text the client keeps.
    if (!final) delta.resize(complete_utf8_prefix_size(delta));
    if (delta.empty()) return;
    if (!open_ || current_ != kind) {
        if (open_) out.push_back({BlockEvent::Type::Close, current_, ""});
        out.push_back({BlockEvent::Type::Open, kind, ""});
        open_ = true;
        current_ = kind;
    }
    sent += delta;
    out.push_back({BlockEvent::Type::Delta, kind, std::move(delta)});
}

std::vector<BlockEvent> BlockStream::update(const std::string & text, const std::string & reasoning) {
    std::vector<BlockEvent> out;
    // Reasoning first: what a model thinks comes before what it answers, so a snapshot that moves
    // both continues the thinking block before it opens the answer.
    append(BlockKind::Reasoning, reasoning, false, out);
    append(BlockKind::Text, text, false, out);
    return out;
}

std::vector<BlockEvent> BlockStream::finish(const std::string & text, const std::string & reasoning) {
    std::vector<BlockEvent> out;
    append(BlockKind::Reasoning, reasoning, true, out);
    append(BlockKind::Text, text, true, out);
    if (open_) out.push_back({BlockEvent::Type::Close, current_, ""});
    open_ = false;
    return out;
}

std::string sse_event(const std::string & name, const json & data) {
    return "event: " + name + "\ndata: " + data.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n";
}

void place_system_messages(std::vector<ChatMessage> & messages) {
    std::vector<ChatMessage> out;
    out.reserve(messages.size());
    bool leading = true;
    for (ChatMessage & m : messages) {
        if (m.role != "system") {
            leading = false;
        } else if (leading) {
            if (m.content.empty()) continue;
            if (!out.empty()) {
                out.back().content += "\n\n" + m.content;
                continue;
            }
        } else {
            m.role = "user";
        }
        out.push_back(std::move(m));
    }
    messages = std::move(out);
}

int estimate_prompt_tokens(const ChatRequest & req) {
    size_t bytes = 0;
    for (const ChatMessage & m : req.messages) {
        bytes += kEstimateMessageOverheadBytes + m.content.size() + m.reasoning.size();
        for (const ChatToolCall & c : m.tool_calls)
            bytes += c.name.size() + c.arguments.size();
    }
    for (const ChatTool & t : req.tools)
        bytes += t.name.size() + t.description.size() + t.parameters.size();
    return (int) ((bytes + kEstimateBytesPerToken - 1) / kEstimateBytesPerToken);
}

} // namespace bmoe::server
