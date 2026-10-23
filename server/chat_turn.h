// One chat turn as the engine host runs it: the conversation, the tools, and the per-request
// engine parameters. What a protocol endpoint parses into, and all the host needs to know of it.
#pragma once

#include "bmoe/chat.h"

#include <string>
#include <vector>

namespace bmoe::server {

struct ChatRequest {
    std::vector<ChatMessage> messages;
    std::vector<ChatTool> tools;
    ToolChoice tool_choice = ToolChoice::Auto;
    int n_predict = 0;
    bool think = false;      // RunConfig::think's default
    std::string json_schema; // serialized schema for this request; empty means unconstrained
};

} // namespace bmoe::server
