// One chat turn as the engine host runs it: the conversation, the tools, and the per-request
// engine parameters. What a protocol endpoint parses into, and all the host needs to know of it.
#pragma once

#include "bmoe/chat.h"
#include "bmoe/session.h"

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
    // The request's own sampling and output controls (request_sampling.h), handed to the engine as
    // GenerateRequest's fields of the same names. Unset: the session's settings.
    std::optional<RequestSampling> sampling;
    std::vector<std::string> stop_text;
    std::string grammar;
    int top_logprobs = 0;  // candidates per token reported beside its own logprob
    bool logprobs = false; // report each token's logprob
    // A plain text completion (/completion, /v1/completions): `prompt` is fed as written, with no
    // template and no messages, from an empty context (GenerateRequest::raw).
    bool raw = false;
    std::string prompt;
};

} // namespace bmoe::server
