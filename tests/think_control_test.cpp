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
//   1. probe_think_control tells the regimes apart — a template that honours the flag; one that
//      ignores it but whose reasoning is structural, so the turn can start past it; and one whose
//      model owns its reasoning span and cannot be asked to skip it. Including the case that is
//      easy to get wrong: a model whose FAMILY declares reasoning tags but which never reasons
//      itself, and so must not be reported uncontrollable.
//   2. the prefill produces a prompt whose reasoning span is already CLOSED. This is the assertion
//      that matters: an opened-and-not-closed span is the opposite request, and it is exactly what
//      asking for an AUTO continuation would silently produce.
//
// This file does name `<think>`, in the LFM2.5 case only, because a test of a specific template is
// entitled to state what that template's span looks like. The ENGINE names no marker for any
// family: everything it emits comes from llama.cpp's handler for the loaded model, and every
// decision it makes reads model-supplied data (`thinking_start_tag`/`thinking_end_tag`). The
// gpt-oss expectations below take the harder route and are derived from that template's own
// thinking-on render, so a submodule bump that changes harmony's channel markers updates them with
// it instead of breaking the test.
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

#if !defined(BMOE_TMPL_QWEN3) || !defined(BMOE_TMPL_LFM25) || !defined(BMOE_TMPL_GPTOSS) ||                            \
    !defined(BMOE_TMPL_LFM2) || !defined(BMOE_TMPL_LFM25_INSTRUCT) || !defined(BMOE_TMPL_GEMMA4)
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
        // renders are identical. The continuation hook does land (the span below really is closed),
        // but LFM2.5 declares <think>/</think>, so the closed span is only a suggestion to a model
        // that owns its own reasoning tags — and measured on-device it reasons straight past it,
        // untagged, into the answer. Reported as uncontrollable rather than made worse.
        {
            auto t = load(BMOE_TMPL_LFM25);
            expect("lfm2.5: flag is inert in the template", render(t.get(), /*think*/ true, /*prefill*/ false) ==
                                                                render(t.get(), /*think*/ false, /*prefill*/ false));
            expect("lfm2.5: probed as none (declares its own reasoning tags)",
                   bmoe::detail::probe_think_control(t.get()) == bmoe::ThinkControl::None);
            // The prefill itself is well-formed — this is why "none" is a statement about the
            // MODEL, not about a mechanism that failed to render.
            expect_span_closed("lfm2.5: the prefill would close the span correctly", t.get(), "</think>");
        }

        // gpt-oss / harmony. Its template ignores the flag too, and it declares NO thinking tags:
        // reasoning is a channel the format separates structurally, so priming past it is not
        // something the model can decline. This is the case the engine used to carry as two
        // hardcoded harmony marker strings in the decode path.
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

        // The non-reasoning members of the same family. Their handler publishes <think>/</think> for
        // the family as a whole, but these templates never emit it, so the models never reason.
        // Reporting them uncontrollable would tell the user "this model always reasons" about a model
        // that never does — and disable a control that was simply moot. They must come out as
        // Template: the flag is passed, and there is nothing to suppress.
        for (const char * p : {BMOE_TMPL_LFM2, BMOE_TMPL_LFM25_INSTRUCT}) {
            auto t = load(p);
            const std::string name = std::string("non-reasoning lfm variant is not reported uncontrollable: ") + p;
            expect(name.c_str(), bmoe::detail::probe_think_control(t.get()) == bmoe::ThinkControl::Template);
        }

        // Gemma 4 reads enable_thinking, so it never reaches the tag test at all — pinned because it
        // ships in the app catalog and declares a thinking tag of its own.
        {
            auto t = load(BMOE_TMPL_GEMMA4);
            expect("gemma4: template honours enable_thinking",
                   bmoe::detail::probe_think_control(t.get()) == bmoe::ThinkControl::Template);
        }

        // A template with no reasoning of any kind. Nothing to suppress, so nothing to report: None
        // is reserved for models that demonstrably DO reason and cannot be stopped.
        {
            auto t = common_chat_templates_init(
                nullptr, "{% for m in messages %}{{ m.role }}: {{ m.content }}\n{% endfor %}assistant:");
            expect("plain template: nothing to suppress, so not reported uncontrollable",
                   bmoe::detail::probe_think_control(t.get()) == bmoe::ThinkControl::Template);
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
