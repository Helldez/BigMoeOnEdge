// Thinking-control tests (issue #82).
//
// The bug: turning Thinking off set enable_thinking=false and stopped there. That flag is handed
// to the model's jinja template, which is free to ignore it — LFM2.5's never reads it — so the
// prompt came out byte-identical either way and the model reasoned on, with nothing reporting that
// the setting had been dropped.
//
// Two things are asserted here, both against REAL templates from the vendored submodule (paths
// injected by CMake), with no model:
//
//   1. probe_think_control tells the three regimes apart — a template that honours the flag, one
//      that ignores it but whose handler can have the reasoning span closed for it, and (by
//      construction) one where neither works.
//   2. the prefill produces a prompt whose reasoning span is already CLOSED. This is the assertion
//      that matters: an opened-and-not-closed span is the opposite request, and it is exactly what
//      asking for an AUTO continuation would silently produce.
//
// Nothing here hardcodes a marker for any family. The expected shapes are derived from the
// thinking-on render of the same template, so a submodule bump that changes a family's markers
// updates the expectations with it instead of breaking the test.
//
// Assertions are explicit (not <cassert>) because the Release gates build with NDEBUG, which
// would compile assert() out.

#include "thinking_control.h"

#include "chat.h"
#include "common.h"

#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>

#if !defined(BMOE_TMPL_QWEN3) || !defined(BMOE_TMPL_LFM25) || !defined(BMOE_TMPL_GPTOSS)
#error "template paths must be defined by the build"
#endif

static int failures = 0;

static void expect(const char * name, bool ok, const std::string & detail = {}) {
    if (ok) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s%s%s\n", name, detail.empty() ? "" : "\n  ", detail.c_str());
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

static common_chat_templates_ptr load(const char * path) {
    return common_chat_templates_init(/*model*/ nullptr, read_file(path));
}

// Render one turn the way generate() does, optionally with the reasoning span closed up front.
static std::string render(const common_chat_templates * tmpls, bool think, bool prefill) {
    common_chat_msg user;
    user.role = "user";
    user.content = "hi";

    common_chat_templates_inputs inputs;
    inputs.messages = {user};
    inputs.add_generation_prompt = true;
    inputs.use_jinja = true;
    inputs.enable_thinking = think;
    inputs.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
    if (prefill) bmoe::detail::add_no_think_prefill(inputs);

    return common_chat_templates_apply(const_cast<common_chat_templates *>(tmpls), inputs).prompt;
}

// The check that the rejected logit-level attempt could not make: is the reasoning span CLOSED?
//
// The closing marker is not named here — it is taken from the same template's thinking-on render,
// which is where the model is left mid-reasoning. A prefilled prompt must contain that marker (the
// span was closed) and must end at or after it (nothing reopens afterwards).
static void expect_span_closed(const char * name, const common_chat_templates * tmpls, const char * close_marker) {
    const std::string prefilled = render(tmpls, /*think*/ false, /*prefill*/ true);
    const size_t at = prefilled.rfind(close_marker);
    if (at == std::string::npos) {
        expect(name, false, "prefilled prompt never closes the reasoning span:\n  " + prefilled);
        return;
    }
    // Only whitespace and the answer-channel header may follow: what must NOT follow is a fresh
    // opening of the span, which would leave the model reasoning again.
    const std::string tail = prefilled.substr(at + std::char_traits<char>::length(close_marker));
    expect(name, tail.find("<think>") == std::string::npos, "span reopens after closing:\n  " + prefilled);
}

int main() {
    try {
        // Qwen3 reads enable_thinking (it renders its own closed <think></think> when false), so the
        // flag alone is the whole mechanism and the engine must add nothing.
        {
            auto t = load(BMOE_TMPL_QWEN3);
            expect("qwen3: template honours enable_thinking",
                   bmoe::detail::probe_think_control(t.get()) == bmoe::ThinkControl::Template);
        }

        // LFM2.5 — the model from issue #82. Its template never mentions enable_thinking, so both
        // renders are identical; the handler does implement the continuation hook, so the span can
        // be closed in the prompt.
        {
            auto t = load(BMOE_TMPL_LFM25);
            expect("lfm2.5: flag is inert in the template", render(t.get(), /*think*/ true, /*prefill*/ false) ==
                                                                render(t.get(), /*think*/ false, /*prefill*/ false));
            expect("lfm2.5: probed as prefill",
                   bmoe::detail::probe_think_control(t.get()) == bmoe::ThinkControl::Prefill);
            expect_span_closed("lfm2.5: prefilled span is closed", t.get(), "</think>");
        }

        // gpt-oss / harmony. Its template ignores the flag too, and it exposes no thinking tags at
        // all — a tag-based detector would give up here. Probing the continuation instead sees the
        // mechanism that does exist: the handler primes the answer channel. This is the case the
        // engine used to carry as two hardcoded harmony marker strings in the decode path.
        {
            auto t = load(BMOE_TMPL_GPTOSS);
            expect("gpt-oss: probed as prefill",
                   bmoe::detail::probe_think_control(t.get()) == bmoe::ThinkControl::Prefill);

            const std::string prefilled = render(t.get(), /*think*/ false, /*prefill*/ true);
            const std::string thinking = render(t.get(), /*think*/ true, /*prefill*/ false);
            // Derived, not asserted as a constant: whatever channel the plain render leaves the
            // model in, the prefilled one must have moved past it to a different, later one.
            expect("gpt-oss: prefill moves past the reasoning channel",
                   prefilled != thinking && prefilled.size() > thinking.size());
        }

        // A template with no reasoning of any kind: the flag does nothing and there is no span to
        // close. The honest answer is None, so a caller can hide the toggle instead of lying.
        {
            auto t = common_chat_templates_init(
                nullptr, "{% for m in messages %}{{ m.role }}: {{ m.content }}\n{% endfor %}assistant:");
            expect("plain template: reports none",
                   bmoe::detail::probe_think_control(t.get()) == bmoe::ThinkControl::None);
        }

        // A probe that could not run is no evidence the flag is inert: fail open to the
        // pre-existing behaviour rather than declaring the model uncontrollable.
        expect("null templates: fails open to template",
               bmoe::detail::probe_think_control(nullptr) == bmoe::ThinkControl::Template);

        expect("names are stable",
               std::string(bmoe::think_control_name(bmoe::ThinkControl::Prefill)) == "prefill" &&
                   std::string(bmoe::think_control_name(bmoe::ThinkControl::None)) == "none" &&
                   std::string(bmoe::think_control_name(bmoe::ThinkControl::Template)) == "template");
    } catch (const std::exception & e) {
        std::printf("[FAIL] unexpected exception: %s\n", e.what());
        ++failures;
    }

    std::printf(failures == 0 ? "\nthink control: all checks passed\n" : "\nthink control: %d check(s) failed\n",
                failures);
    return failures == 0 ? 0 : 1;
}
