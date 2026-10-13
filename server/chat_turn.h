// One chat turn as the engine host runs it: the conversation, the tools, and the two per-request
// engine parameters. What a protocol endpoint parses into, and all the host needs to know of it.
#pragma once

#include "bmoe/chat.h"

#include <vector>

namespace bmoe::server {

struct ChatRequest {
    std::vector<ChatMessage> messages;
    std::vector<ChatTool> tools;
    ToolChoice tool_choice = ToolChoice::Auto;
    int n_predict = 0;
    bool think = true;
};

} // namespace bmoe::server
