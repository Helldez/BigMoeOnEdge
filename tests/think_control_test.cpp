// Regression test for the thinking-control probe (issue #82).
//
// The bug: `--no-think` set the template's enable_thinking kwarg and stopped there. A template
// that never reads the variable (LFM2.5) renders the same prompt either way, so the request was
// discarded in silence and a reasoning model spent its budget reasoning anyway. Worse, nothing
// could tell the difference — llama.cpp's own `supports_thinking` is a hardcoded per-handler
// literal meaning "this model can reason", not "this template reads the flag".
//
// The fix answers it empirically: render the template with thinking on and off and compare. This
// drives that classifier against three real vendored templates, one per regime, with no model —
// the whole point is that the decision is testable without a gguf. Assertions are explicit (not
// <cassert>) because the Release gates build with NDEBUG, which would compile assert() out.

#include "thinking_control.h"

#include "chat_parse.h"

#include "chat.h"
#include "common.h"

#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>

// Real templates from the vendored submodule (paths injected by CMake). Hand-rolled stubs do not
// reliably reproduce how the handlers populate thinking tags, so the genuine files are used; a
// submodule bump that moves or changes them fails here loudly rather than rotting.
#if !defined(BMOE_QWEN3_TEMPLATE_PATH) || !defined(BMOE_LFM25_TEMPLATE_PATH) || !defined(BMOE_GPTOSS_TEMPLATE_PATH)
#error "template paths must be defined by the build"
#endif

static int failures = 0;

static void expect_true(const char * name, bool cond) {
    if (cond) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

static void expect_ctl(const char * name, bmoe::ThinkControl got, bmoe::ThinkControl want) {
    if (got == want) {
        std::printf("[PASS] %s (%s)\n", name, bmoe::think_control_name(got));
    } else {
        std::printf("[FAIL] %s\n  got:  %s\n  want: %s\n", name, bmoe::think_control_name(got),
                    bmoe::think_control_name(want));
        ++failures;
    }
}

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

// Probe a template file the way a session would, minus the model: no vocab, so tag usability is
// taken on trust (a loaded model tightens that by checking the tags tokenize).
static bmoe::ThinkControl probe_file(const char * path) {
    const std::string tmpl = read_file(path);
    if (tmpl.empty()) {
        return bmoe::ThinkControl::None;
    }
    auto tmpls = common_chat_templates_init(/*model=*/nullptr, tmpl);
    return bmoe::detail::probe_think_control(tmpls.get(), /*vocab=*/nullptr);
}

// The applied params for a template, to assert what the classifier saw.
static common_chat_params apply(const char * path, bool enable_thinking) {
    const std::string tmpl = read_file(path);
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
    using bmoe::ThinkControl;

    // Qwen3: reads enable_thinking and prefills the opening tag when on, so the two renders differ
    // and the request is honoured where it is cheapest — in the prompt.
    expect_ctl("qwen3 template honours enable_thinking", probe_file(BMOE_QWEN3_TEMPLATE_PATH), ThinkControl::Template);
    expect_true("qwen3 renders differ across the flag",
                apply(BMOE_QWEN3_TEMPLATE_PATH, true).prompt != apply(BMOE_QWEN3_TEMPLATE_PATH, false).prompt);

    // LFM2.5: the case from issue #82. The flag changes nothing, but the model declares where its
    // reasoning block begins and ends, which is enough to enforce the request while decoding.
    expect_ctl("lfm2.5 needs decode-time enforcement", probe_file(BMOE_LFM25_TEMPLATE_PATH), ThinkControl::Sampler);
    {
        const common_chat_params on = apply(BMOE_LFM25_TEMPLATE_PATH, true);
        const common_chat_params off = apply(BMOE_LFM25_TEMPLATE_PATH, false);
        expect_true("lfm2.5 renders are identical across the flag (the silent no-op)", on.prompt == off.prompt);
        expect_true("lfm2.5 exposes thinking tags to enforce against",
                    !off.thinking_start_tag.empty() && !off.thinking_end_tag.empty());
    }

    // gpt-oss: no thinking tags at all; suppression happens by priming the final channel in the
    // prompt, which generate() keys on the harmony marker to do.
    expect_ctl("gpt-oss uses the primed final channel", probe_file(BMOE_GPTOSS_TEMPLATE_PATH),
               ThinkControl::ForcedFinal);

    // The harmony marker check itself: trailing whitespace must not hide it, and a chatml tail
    // must not be mistaken for it (that would prime a channel no other family understands).
    expect_true("harmony marker survives trailing whitespace",
                bmoe::detail::ends_with_harmony_assistant("...<|start|>assistant\n  "));
    expect_true("chatml tail is not harmony", !bmoe::detail::ends_with_harmony_assistant("<|im_start|>assistant\n"));
    {
        std::string trimmed;
        bmoe::detail::ends_with_harmony_assistant("abc<|start|>assistant \n", &trimmed);
        expect_true("harmony check returns the right-trimmed prompt", trimmed == "abc<|start|>assistant");
    }

    // The fourth regime: a plain template that ignores the flag and declares no reasoning tags.
    // Nothing can honour the request here, and saying so is the entire point — an engine that
    // reported "template" would be repeating the lie issue #82 was filed about.
    {
        const std::string plain = "{% for m in messages %}<|im_start|>{{ m.role }}\n{{ m.content }}<|im_end|>\n"
                                  "{% endfor %}{% if add_generation_prompt %}<|im_start|>assistant\n{% endif %}";
        auto tmpls = common_chat_templates_init(/*model=*/nullptr, plain);
        expect_ctl("a template with no thinking support reports none",
                   bmoe::detail::probe_think_control(tmpls.get(), nullptr), ThinkControl::None);
    }

    // Fail open on the defensive path: with nothing to probe, assume the template honours the
    // flag. Claiming "your switch does nothing" on a guess is worse than staying quiet.
    expect_ctl("null templates fail open", bmoe::detail::probe_think_control(nullptr, nullptr), ThinkControl::Template);

    // The block the enforcement actually produces must parse. Forcing the close the instant the
    // model opens the block yields an EMPTY reasoning span, which is a different shape from the
    // one the parser sees in normal use — if it does not match, the markers leak into the answer
    // verbatim and the user reads "<think></think>" instead of a clean reply.
    {
        const common_chat_params cp = apply(BMOE_LFM25_TEMPLATE_PATH, false);
        common_chat_parser_params pp = bmoe::detail::build_parse_params(cp);
        // Whitespace before the opening tag is the shape that bites: the parser's reasoning rule is
        // anchored right after the generation prompt and is optional, so one leading space makes it
        // silently not match and the markers land in the answer. That is what reasoning_prefix_offset
        // exists to absorb, so parse through it exactly as session.cpp does.
        for (const char * raw : {"<think></think>Hello", "<think>\n</think>Hello", "<think> </think>Hello",
                                 "\n<think></think>Hello", " <think></think>Hello", "  \n<think> </think>Hello"}) {
            const std::string s = raw;
            const size_t skip = bmoe::detail::reasoning_prefix_offset(s, cp.thinking_start_tag);
            common_chat_msg m;
            bool threw = false;
            try {
                m = common_chat_parse(skip ? s.substr(skip) : s, /*is_partial=*/false, pp);
            } catch (const std::exception & e) {
                threw = true;
                std::printf("  parse threw on \"%s\": %s\n", raw, e.what());
            }
            expect_true("forced-close block parses", !threw);
            expect_true("forced-close block leaves no markers in the answer", m.content == "Hello");
        }

        // The trim is surgical: it fires only when whitespace is all that separates the stream from
        // the opening tag. An answer that legitimately starts with a blank line keeps it.
        expect_true("ordinary leading whitespace is preserved",
                    bmoe::detail::reasoning_prefix_offset("\n  Hello", cp.thinking_start_tag) == 0);
        expect_true("no trim when the block opens the stream",
                    bmoe::detail::reasoning_prefix_offset("<think></think>Hi", cp.thinking_start_tag) == 0);
        expect_true("no tag, no trim", bmoe::detail::reasoning_prefix_offset(" <think>x", "") == 0);

        // The streaming path. The app renders the partial parse token by token, so what the user
        // reads mid-stream is this, not the final message: an opening tag whose block has not
        // closed yet must not be shown as answer text.
        for (const char * raw : {"<think>", "<think></think>", "<think></think>Hel"}) {
            common_chat_msg m;
            bool threw = false;
            try {
                m = common_chat_parse(raw, /*is_partial=*/true, pp);
            } catch (const std::exception & e) {
                threw = true;
                std::printf("  partial parse threw on \"%s\": %s\n", raw, e.what());
            }
            std::printf("  partial raw=\"%s\" -> content=\"%s\" reasoning=\"%s\"%s\n", raw, m.content.c_str(),
                        m.reasoning_content.c_str(), threw ? " (THREW)" : "");
            expect_true("streamed forced close shows no markers as answer text",
                        !threw && m.content.find("think>") == std::string::npos);
        }
    }

    // The wire names are a protocol contract (docs/telemetry.md), not a debug string.
    expect_true("wire names are stable",
                std::string(bmoe::think_control_name(ThinkControl::Template)) == "template" &&
                    std::string(bmoe::think_control_name(ThinkControl::ForcedFinal)) == "forced_final" &&
                    std::string(bmoe::think_control_name(ThinkControl::Sampler)) == "sampler" &&
                    std::string(bmoe::think_control_name(ThinkControl::None)) == "none");

    if (failures == 0) {
        std::printf("all thinking-control checks passed\n");
        return 0;
    }
    std::printf("%d thinking-control check(s) failed\n", failures);
    return 1;
}
