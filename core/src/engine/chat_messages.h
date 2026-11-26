// bmoe/chat.h <-> llama.cpp's chat types, in one place.
//
// The template, the tool-call syntax of each model family and the parser that reads a call back out
// of the generated text all belong to llama.cpp's `common/chat`. Nothing here knows a model or a
// format: these functions only move fields between the engine's public structs and that layer's,
// so the one header that names `common_chat_*` for tools is this one.
#pragma once

#include "bmoe/chat.h"

#include "chat.h"

#include <optional>
#include <vector>

namespace bmoe::detail {

std::vector<common_chat_msg> to_common_messages(const std::vector<ChatMessage> & messages);

// Whether the template takes several tool calls in one turn (its supports_parallel_tool_calls): what
// a request that does not say gets. A property of the model, so the session reads it once at open().
bool template_parallel_tool_calls(const common_chat_templates * tmpls);

// Describe `tools` to the template and say whether the model may, must or may not call them, and
// whether more than once in the turn: `parallel` when the request says, else `template_parallel`.
// A request with no tools leaves `inputs` as it was.
void add_tools(common_chat_templates_inputs & inputs,
               const std::vector<ChatTool> & tools,
               ToolChoice choice,
               std::optional<bool> parallel,
               bool template_parallel);

std::vector<ChatToolCall> from_common_tool_calls(const std::vector<common_chat_tool_call> & calls);

} // namespace bmoe::detail
