// Regression test for the reasoning-parser wiring (issue #49).
//
// The bug: session.cpp built parse params with `common_chat_parser_params(cp)`, whose
// convenience constructor copies only format/generation_prompt and leaves the PEG grammar
// arena empty. `common_chat_parse` then throws on the first token, a catch swallows it, and
// the raw stream — reasoning markers and all — is shown verbatim. The fix copies the grammar
// arena across (`pp.parser = cp.parser`), extracted into bmoe::detail::build_parse_params.
//
// This exercises that exact function against a Qwen3-style template, with no model: the whole
// point is that the wiring is testable without a gguf. Assertions are explicit (not <cassert>)
// because the Release gates build with NDEBUG, which would compile assert() out.

#include "chat_parse.h"
#include "stop_text.h"

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
static common_chat_params apply_template(bool enable_thinking, const std::vector<common_chat_tool> & tools = {}) {
    static const std::string tmpl = read_file(BMOE_QWEN3_TEMPLATE_PATH);
    auto tmpls = common_chat_templates_init(/*model=*/nullptr, tmpl);

    common_chat_msg user;
    user.role = "user";
    user.content = "hi";

    common_chat_templates_inputs inputs;
    inputs.messages = {user};
    inputs.tools = tools;
    inputs.add_generation_prompt = true;
    inputs.use_jinja = true;
    inputs.enable_thinking = enable_thinking;
    inputs.reasoning_format = COMMON_REASONING_FORMAT_AUTO;

    return common_chat_templates_apply(tmpls.get(), inputs);
}

static bool starts_with(const std::string & s, const std::string & prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

// The per-token parse of a call being written (TokenMetrics::tool_calls) is what every endpoint
// streams by, and clients append the pieces: so the partial calls must only ever grow by
// appending, from the first token that shows one to the final parse. This pins that contract on a
// real template, so a submodule bump that broke it fails here rather than in a client.
static void partial_tool_call_checks() {
    common_chat_tool write_file;
    write_file.name = "write_file";
    write_file.description = "Write a file";
    write_file.parameters = R"({"type":"object","properties":{"path":{"type":"string"},)"
                            R"("content":{"type":"string"}},"required":["path","content"]})";
    common_chat_params cp = apply_template(/*enable_thinking=*/true, {write_file});
    common_chat_parser_params pp = bmoe::detail::build_parse_params(cp);

    const std::string out = "Writing it.\n</think>\n\n"
                            "<tool_call>\n<function=write_file>\n"
                            "<parameter=path>\npage.html\n</parameter>\n"
                            "<parameter=content>\n<p>caf\xC3\xA9</p>\n<p>two</p>\n</parameter>\n"
                            "</function>\n</tool_call>";
    std::vector<common_chat_tool_call> prev;
    bool grows = true, threw = false, seen_before_end = false;
    for (size_t n = 1; n <= out.size(); n += 3) {
        common_chat_msg m;
        try {
            m = common_chat_parse(common_chat_input(out.substr(0, n)), /*is_partial=*/true, pp);
        } catch (const std::exception & e) {
            threw = true;
            std::printf("  partial parse threw at %zu bytes: %s\n", n, e.what());
            break;
        }
        if (m.tool_calls.size() < prev.size()) grows = false;
        for (size_t i = 0; i < prev.size() && i < m.tool_calls.size(); ++i) {
            if (!starts_with(m.tool_calls[i].arguments, prev[i].arguments)) grows = false;
            if (!prev[i].arguments.empty() && m.tool_calls[i].name != prev[i].name) grows = false;
        }
        if (!m.tool_calls.empty() && !m.tool_calls[0].arguments.empty() && n + 3 <= out.size()) seen_before_end = true;
        prev = m.tool_calls;
    }
    common_chat_msg fin = common_chat_parse(common_chat_input(out), /*is_partial=*/false, pp);
    expect_true("partial tool-call parse does not throw", !threw);
    expect_true("a call is visible while it is being written", seen_before_end);
    expect_true("partial calls only grow by appending, and keep their name once arguments begin", grows);
    expect_true("the final call extends the last partial one",
                fin.tool_calls.size() == 1 && !prev.empty() &&
                    starts_with(fin.tool_calls[0].arguments, prev[0].arguments));
    expect_eq("the final call is the one written", fin.tool_calls.empty() ? "" : fin.tool_calls[0].name, "write_file");
}

// A stop is matched in the answer only (bmoe::detail::AnswerStart, the session's emit loop): the
// generated text is fed as the session feeds it, a few bytes at a time through the real partial
// parse, and the stop the reasoning contains must not end the turn. Reported from an API client: a
// model reasoned about the stop word, the turn ended there, and the answer came back empty.
static std::string stopped_answer(const common_chat_parser_params & pp,
                                  const std::string & out,
                                  const std::vector<std::string> & stops,
                                  bool & hit,
                                  bool & shown_clean) {
    bmoe::detail::AnswerStart start;
    std::string gen;
    hit = false;
    shown_clean = true;
    for (size_t n = 0; n < out.size() && !hit; n += 3) {
        const size_t before = gen.size();
        gen += out.substr(n, 3);
        size_t shown = gen.size();
        if (!start.open) {
            const common_chat_msg m = common_chat_parse(common_chat_input(gen), /*is_partial=*/true, pp);
            start.note(gen, m.content);
        }
        const size_t at = start.open ? bmoe::detail::find_stop(gen, stops, before, start.from) : std::string::npos;
        if (at != std::string::npos) {
            gen.resize(at);
            shown = at;
            hit = true;
        } else if (start.open) {
            shown -= bmoe::detail::stop_prefix_tail(gen, stops, start.from);
        }
        const common_chat_msg view = common_chat_parse(common_chat_input(gen.substr(0, shown)), true, pp);
        for (const std::string & st : stops)
            if (view.content.find(st) != std::string::npos) shown_clean = false;
    }
    return common_chat_parse(common_chat_input(gen), /*is_partial=*/false, pp).content;
}

static void stop_in_answer_checks() {
    const common_chat_parser_params pp = bmoe::detail::build_parse_params(apply_template(/*enable_thinking=*/true));
    bool hit = false, clean = false;
    const std::string reasoned = "They want me to stop at END.\nEND is the word.\n</think>\n\nOne two END three";
    expect_eq("stop: a stop inside the reasoning does not end the turn, the one in the answer does",
              stopped_answer(pp, reasoned, {"END"}, hit, clean), "One two ");
    expect_true("stop: the answer's stop is hit and never shown while it forms", hit && clean);
    expect_eq("stop: a stop only the reasoning contains leaves the answer whole",
              stopped_answer(pp, "Say END? No.\n</think>\n\nOne two three", {"END"}, hit, clean), "One two three");
    expect_true("stop: nothing was hit", !hit);

    // No reasoning before the answer: the stop is matched from the first byte, as before.
    const common_chat_parser_params plain = bmoe::detail::build_parse_params(apply_template(/*enable_thinking=*/false));
    expect_eq("stop: an answer with no reasoning stops as before",
              stopped_answer(plain, "One two END three", {"\x01never", "END"}, hit, clean), "One two ");
    expect_true("stop: hit at the answer's own stop", hit && clean);

    // The helpers' floor: a match that begins before it is not one, and nothing before it is held back.
    const std::vector<std::string> stops = {"abc"};
    expect_true("stop: a match straddling the answer's start is not a stop",
                bmoe::detail::find_stop("xabc", stops, 0, 2) == std::string::npos &&
                    bmoe::detail::find_stop("xabcabc", stops, 0, 2) == 4 &&
                    bmoe::detail::stop_prefix_tail("xab", stops, 2) == 0 &&
                    bmoe::detail::stop_prefix_tail("xab", stops, 0) == 2);
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

    // Positive: the production wiring copies the grammar and strips the reasoning block.
    common_chat_parser_params good = bmoe::detail::build_parse_params(cp);
    common_chat_msg parsed;
    bool threw = false;
    try {
        parsed = common_chat_parse(common_chat_input(raw), /*is_partial=*/false, good);
    } catch (const std::exception & e) {
        threw = true;
        std::printf("  build_parse_params path threw: %s\n", e.what());
    }
    expect_true("build_parse_params: parse does not throw", !threw);
    expect_eq("build_parse_params: reasoning stripped from content", parsed.content, want_content);
    // The parser preserves the reasoning span verbatim (a trailing newline before </think> may
    // survive); the answer being clean is the contract, so compare the reasoning trimmed.
    expect_eq("build_parse_params: reasoning captured", rstrip(parsed.reasoning_content), want_reasoning);

    // Negative control: the pre-fix wiring (convenience ctor, no arena copied) must NOT correctly
    // strip — whether it throws or returns the raw stream, it must not yield the clean answer.
    // This pins bug #49 so a future submodule bump that silently changes the behaviour is noticed.
    common_chat_parser_params unloaded(cp);
    bool unloaded_stripped = false;
    try {
        unloaded_stripped =
            common_chat_parse(common_chat_input(raw), /*is_partial=*/false, unloaded).content == want_content;
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
        partial = common_chat_parse(common_chat_input("I'm\nthink"), /*is_partial=*/true, good);
    } catch (const std::exception &) {
        partial_threw = true;
    }
    expect_true("partial parse mid-reasoning does not throw", !partial_threw);
    expect_true("partial parse leaks no reasoning into content", partial.content.empty());
    expect_true("partial parse exposes the reasoning span for a live thinking block",
                !partial.reasoning_content.empty());

    partial_tool_call_checks();
    stop_in_answer_checks();

    if (failures == 0) {
        std::printf("all chat-parse checks passed\n");
        return 0;
    }
    std::printf("%d chat-parse check(s) failed\n", failures);
    return 1;
}
