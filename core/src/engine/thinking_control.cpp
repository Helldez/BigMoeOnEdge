#include "thinking_control.h"

#include <cstdio>
#include <exception>
#include <string>

namespace bmoe {

const char * think_control_name(ThinkControl c) {
    switch (c) {
    case ThinkControl::Template:
        return "template";
    case ThinkControl::Prefill:
        return "prefill";
    case ThinkControl::None:
        return "none";
    }
    return "template";
}

} // namespace bmoe

namespace bmoe::detail {

namespace {

// What a natively-supporting template puts inside the closed span. See add_no_think_prefill:
// whitespace, so the engine contributes no words to the model's own reasoning.
const char * const kEmptyReasoning = "\n\n";

// Render a one-turn conversation the way generate() renders a real one — same jinja path, same
// reasoning format — so what the probe observes is what generation will produce. The message text
// is irrelevant: templates branch on the flag, never on the words.
common_chat_params apply_probe(const common_chat_templates * tmpls, bool enable_thinking, bool prefill) {
    common_chat_msg user;
    user.role = "user";
    user.content = "hi";

    common_chat_templates_inputs inputs;
    inputs.messages = {user};
    inputs.add_generation_prompt = true;
    inputs.use_jinja = true;
    inputs.enable_thinking = enable_thinking;
    inputs.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
    if (prefill) add_no_think_prefill(inputs);

    return common_chat_templates_apply(const_cast<common_chat_templates *>(tmpls), inputs);
}

} // namespace

void add_no_think_prefill(common_chat_templates_inputs & inputs) {
    common_chat_msg prefill;
    prefill.role = "assistant";
    prefill.reasoning_content = kEmptyReasoning;
    // content stays empty: the handler renders the closing tag and then nothing, leaving the model
    // at the first token of its answer. Nothing is appended that would have to be stripped back off.
    inputs.messages.push_back(prefill);
    inputs.continue_final_message = COMMON_CHAT_CONTINUATION_CONTENT;
}

ThinkControl probe_think_control(const common_chat_templates * tmpls) {
    if (tmpls == nullptr) return ThinkControl::Template;
    try {
        const common_chat_params off_p = apply_probe(tmpls, /*enable_thinking=*/false, /*prefill=*/false);
        const std::string & off = off_p.prompt;
        const std::string on = apply_probe(tmpls, /*enable_thinking=*/true, /*prefill=*/false).prompt;
        if (on != off) return ThinkControl::Template;

        // Does the model own a reasoning span — a `<think>`-style pair it opens and closes itself?
        //
        // Both halves are needed. A declared tag alone proves nothing, because handlers publish the
        // pair for a whole family: the non-reasoning members (LFM2-8B-A1B, LFM2.5-Instruct) advertise
        // `<think>` they never emit. Requiring the template to actually USE it is the same test
        // llama.cpp applies before wiring up reasoning extraction for that family.
        const std::string src = common_chat_templates_source(tmpls);
        const bool owns_span =
            !off_p.thinking_start_tag.empty() && src.find(off_p.thinking_start_tag) != std::string::npos;

        // It does, and the flag is inert: the request cannot be honoured. Closing the span in the
        // prompt is only a suggestion to a model that opens its own — LFM2.5 reasons straight past a
        // pre-closed empty one and emits the reasoning untagged into the answer (issue #82), worse
        // than leaving it alone. Say so instead of making it worse.
        if (owns_span) return ThinkControl::None;

        const std::string prefilled = apply_probe(tmpls, /*enable_thinking=*/false, /*prefill=*/true).prompt;

        // No span of its own, and the prefill lands: reasoning is structural — a channel the format
        // itself separates — so starting the turn past that section is not something the model can
        // decline (harmony/gpt-oss).
        if (prefilled != off && off_p.thinking_start_tag.empty()) return ThinkControl::Prefill;

        // Nothing indicates this model reasons at all: no span it uses, no reasoning section to start
        // past. There is nothing to suppress, so pass the flag and add nothing — claiming it "always
        // reasons" would be a louder lie than the silence this probe exists to end.
        return ThinkControl::Template;
    } catch (const std::exception & e) {
        std::fprintf(stderr, "bmoe: thinking-control probe failed (%s); assuming the template honours it\n", e.what());
        return ThinkControl::Template;
    }
}

} // namespace bmoe::detail
