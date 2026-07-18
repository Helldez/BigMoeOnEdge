// Regression test for the reasoning-parser wiring (issue #49).
//
// The bug: session.cpp built parse params with `common_chat_parser_params(cp)`, whose
// convenience constructor copies only format/generation_prompt and leaves the PEG grammar
// arena empty. `common_chat_parse` then throws on the first token, a catch swallows it, and
// the raw stream — reasoning markers and all — is shown verbatim. The fix loads the grammar
// (`parser.load(cp.parser)`), extracted into bmoe::detail::build_parse_params.
//
// This exercises that exact function against a Qwen3-style template, with no model: the whole
// point is that the wiring is testable without a gguf. Assertions are explicit (not <cassert>)
// because the Release gates build with NDEBUG, which would compile assert() out.

#include "chat_parse.h"

#include "chat.h"
#include "common.h"

#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// A real Qwen3 chat template, read from the vendored submodule at test time (the path is injected
// by CMake). It is detected as a tag-based reasoning format — the exact template upstream's own
// reasoning tests exercise (tests/test-chat.cpp, message_assist_thoughts). A hand-rolled stub does
// not reliably trip the differential analyzer, so the test uses the genuine file; if a submodule
// bump removes or moves it, the test fails loudly rather than rotting.
//
// This template PREFILLS "<think>\n" into the thinking-enabled generation prompt, so a model's
// stream begins with the reasoning text and closes it with "</think>" — hence the input below has
// no leading "<think>". common_chat_parse prepends the generation prompt before matching, so the
// prefilled opener is accounted for and the reasoning span is stripped from the shown content.
#ifndef BMOE_QWEN3_TEMPLATE_PATH
#error "BMOE_QWEN3_TEMPLATE_PATH must be defined by the build (path to the vendored Qwen3 .jinja)"
#endif

static int failures = 0;

static std::string read_file(const char * path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("[FAIL] cannot open template: %s\n", path);
        ++failures;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void expect_eq(const char * name, const std::string & got, const std::string & want) {
    if (got == want) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n  got:  \"%s\"\n  want: \"%s\"\n", name, got.c_str(), want.c_str());
        ++failures;
    }
}

static std::string rstrip(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

static void expect_true(const char * name, bool cond) {
    if (cond) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

// Render a one-turn conversation and return the applied chat params (prompt + serialized grammar).
static common_chat_params apply_template(bool enable_thinking) {
    static const std::string tmpl = read_file(BMOE_QWEN3_TEMPLATE_PATH);
    auto tmpls = common_chat_templates_init(/*model=*/nullptr, tmpl);

    common_chat_msg user;
    user.role = "user";
    user.content = "hi";

    common_chat_templates_inputs inputs;
    inputs.messages = {user};
    inputs.add_generation_prompt = true;
    inputs.use_jinja = true;
    inputs.enable_thinking = enable_thinking;
    inputs.reasoning_format = COMMON_REASONING_FORMAT_AUTO;

    return common_chat_templates_apply(tmpls.get(), inputs);
}

int main() {
    // The reasoning stream: the template prefills the opening "<think>\n", so the model emits the
    // reasoning text, closes it with "</think>", then the answer. Mirrors message_assist_thoughts
    // in tests/test-chat.cpp.
    const std::string raw = "I'm\nthinking\n</think>\n\nHello, world!\nWhat's up?";
    const std::string want_content = "Hello, world!\nWhat's up?";
    const std::string want_reasoning = "I'm\nthinking";

    common_chat_params cp = apply_template(/*enable_thinking=*/true);

    // The template must be detected as a tag-based reasoning format, or the test proves nothing.
    expect_true("template yields a non-empty parser grammar", !cp.parser.empty());

    // Positive: the production wiring loads the grammar and strips the reasoning block.
    common_chat_parser_params good = bmoe::detail::build_parse_params(cp);
    common_chat_msg parsed;
    bool threw = false;
    try {
        parsed = common_chat_parse(raw, /*is_partial=*/false, good);
    } catch (const std::exception & e) {
        threw = true;
        std::printf("  build_parse_params path threw: %s\n", e.what());
    }
    expect_true("build_parse_params: parse does not throw", !threw);
    expect_eq("build_parse_params: reasoning stripped from content", parsed.content, want_content);
    // The parser preserves the reasoning span verbatim (a trailing newline before </think> may
    // survive); the answer being clean is the contract, so compare the reasoning trimmed.
    expect_eq("build_parse_params: reasoning captured", rstrip(parsed.reasoning_content), want_reasoning);

    // Negative control: the pre-fix wiring (convenience ctor, no parser.load) must NOT correctly
    // strip — whether it throws or returns the raw stream, it must not yield the clean answer.
    // This pins bug #49 so a future submodule bump that silently changes the behaviour is noticed.
    common_chat_parser_params unloaded(cp);
    unloaded.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
    bool unloaded_stripped = false;
    try {
        unloaded_stripped = common_chat_parse(raw, /*is_partial=*/false, unloaded).content == want_content;
    } catch (const std::exception &) {
        unloaded_stripped = false;
    }
    expect_true("empty-arena ctor does not strip reasoning (the #49 bug)", !unloaded_stripped);

    // Partial parse (the streaming path): mid-reasoning, before "</think>", the answer is not yet
    // available and no reasoning must leak into the shown content — but the reasoning span itself
    // MUST be exposed so a UI can stream it into a thinking block instead of sitting blank while
    // the model reasons (issue #70). That is what session.cpp's shown_view surfaces per token.
    bool partial_threw = false;
    common_chat_msg partial;
    try {
        partial = common_chat_parse("I'm\nthink", /*is_partial=*/true, good);
    } catch (const std::exception &) {
        partial_threw = true;
    }
    expect_true("partial parse mid-reasoning does not throw", !partial_threw);
    expect_true("partial parse leaks no reasoning into content", partial.content.empty());
    expect_true("partial parse exposes the reasoning span for a live thinking block",
                !partial.reasoning_content.empty());

    if (failures == 0) {
        std::printf("all chat-parse checks passed\n");
        return 0;
    }
    std::printf("%d chat-parse check(s) failed\n", failures);
    return 1;
}
