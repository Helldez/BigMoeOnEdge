// A conversation as a front-end hands it to the engine: messages, the tools the model may call, and
// the calls it made.
//
// Pure data, no llama.cpp type and no JSON library: a front-end that speaks a chat protocol (the
// server's OpenAI endpoint) translates into these at its own edge, and the session translates them
// into the template engine's types at its own (core/src/engine/chat_messages.h). The two JSON
// payloads a tool carries - its parameter schema, and a call's arguments - travel as text, because
// the engine never reads them: the model's chat template does.
#pragma once

#include <string>
#include <vector>

namespace bmoe {

// One call the model asked for. `arguments` is the JSON object the model wrote, as text.
struct ChatToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

// One message of a conversation. `role` is system | user | assistant | tool.
struct ChatMessage {
    std::string role;
    std::string content;
    std::string reasoning;                // assistant: the thinking span, when the front-end kept it
    std::vector<ChatToolCall> tool_calls; // assistant: the calls this turn made
    std::string tool_call_id;             // tool: the call this message answers
    std::string name;                     // tool: the function that produced it
};

// A function the model may call. `parameters` is its JSON schema, as text.
struct ChatTool {
    std::string name;
    std::string description;
    std::string parameters;
};

enum class ToolChoice {
    Auto,     // the model decides whether to call
    None,     // the tools are described and may not be called
    Required, // the turn must be a call
};

} // namespace bmoe
