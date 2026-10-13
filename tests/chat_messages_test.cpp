// Tool calls through the engine's chat types, with no model: bmoe/chat.h messages and tools go
// into a real vendored template, and a call the model would write comes back out as a ChatToolCall.
//
// What is pinned here is the wiring the session relies on (core/src/engine/chat_messages.h), not
// any model's syntax - that is llama.cpp's, and its own tests cover it. If a submodule bump changes
// how a template renders tools or what the parser returns for a call, this is where it shows.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "bmoe/chat.h"
#include "chat_messages.h"
#include "chat_parse.h"
#include "thinking_control.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace bmoe;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

static std::string read_file(const char * path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool has(const std::string & hay, const char * needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    const std::string tmpl = read_file(BMOE_QWEN3_TEMPLATE_PATH);
    check(!tmpl.empty(), "the vendored template is readable");
    auto tmpls = common_chat_templates_init(/*model=*/nullptr, tmpl);

    const std::vector<ChatTool> tools = {
        {"read_file", "Read a file from disk",
         R"j({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})j"}};

    // ── a first turn with tools on offer ──
    std::vector<ChatMessage> convo = {{"system", "You fix code.", "", {}, "", ""},
                                      {"user", "What is in calc.py?", "", {}, "", ""}};
    common_chat_templates_inputs inputs;
    detail::build_turn_inputs(inputs, detail::to_common_messages(convo), /*think*/ false, ThinkControl::Template);
    detail::add_tools(inputs, tools, ToolChoice::Auto);
    common_chat_params cp = common_chat_templates_apply(tmpls.get(), inputs);
    check(has(cp.prompt, "read_file") && has(cp.prompt, "Read a file from disk") && has(cp.prompt, "You fix code."),
          "the template describes the tool and keeps the system message");

    // No tools on offer: the same conversation renders without them.
    common_chat_templates_inputs bare;
    detail::build_turn_inputs(bare, detail::to_common_messages(convo), false, ThinkControl::Template);
    detail::add_tools(bare, {}, ToolChoice::Auto);
    check(!has(common_chat_templates_apply(tmpls.get(), bare).prompt, "read_file"),
          "a request with no tools mentions none");

    // ── the model's call, read back ──
    const common_chat_parser_params pp = detail::build_parse_params(cp);
    const std::string raw =
        "<tool_call>\n<function=read_file>\n<parameter=path>\ncalc.py\n</parameter>\n</function>\n</tool_call>";
    const std::string raw_json =
        "<tool_call>\n{\"name\": \"read_file\", \"arguments\": {\"path\": \"calc.py\"}}\n</tool_call>";
    std::vector<ChatToolCall> calls;
    std::string content = "?";
    // The call syntax is the template's own; which of the two this template uses is not this
    // test's to know, so it takes the one the parser accepts.
    for (const std::string & candidate : {raw, raw_json}) {
        try {
            const common_chat_msg msg = common_chat_parse(candidate, /*is_partial*/ false, pp);
            if (msg.tool_calls.empty()) continue;
            calls = detail::from_common_tool_calls(msg.tool_calls);
            content = msg.content;
            // Mid-call, the streaming view must not show the call's text as the answer.
            const common_chat_msg part = common_chat_parse(candidate.substr(0, candidate.size() / 2), true, pp);
            check(!has(part.content, "read_file") && !has(part.content, "tool_call"),
                  "a call in progress does not leak into the answer");
            break;
        } catch (const std::exception &) {
        }
    }
    check(calls.size() == 1 && calls[0].name == "read_file", "the call is parsed out of the generation");
    check(calls.size() == 1 && has(calls[0].arguments, "calc.py") && calls[0].arguments.front() == '{',
          "its arguments are a JSON object naming the file");
    check(content.empty(), "a turn that only calls has no answer text");

    // ── the next turn: the call and the tool's answer go back in ──
    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.tool_calls = {{"call_1", "read_file", calls.empty() ? "{}" : calls[0].arguments}};
    ChatMessage result;
    result.role = "tool";
    result.tool_call_id = "call_1";
    result.name = "read_file";
    result.content = "def median(values): ...";
    convo.push_back(assistant);
    convo.push_back(result);
    common_chat_templates_inputs next;
    detail::build_turn_inputs(next, detail::to_common_messages(convo), false, ThinkControl::Template);
    detail::add_tools(next, tools, ToolChoice::Auto);
    const std::string p2 = common_chat_templates_apply(tmpls.get(), next).prompt;
    check(has(p2, "def median(values): ...") && has(p2, "calc.py"),
          "the earlier call and the tool's answer are rendered into the next prompt");
    // What makes the KV reusable across an agent's steps: the next prompt begins with everything
    // the previous one held, up to where its answer started.
    check(p2.compare(0, cp.prompt.size() - cp.generation_prompt.size(), cp.prompt, 0,
                     cp.prompt.size() - cp.generation_prompt.size()) == 0,
          "the next prompt extends the previous one, so its prefix is reusable");

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall chat message checks passed\n");
    return 0;
}
