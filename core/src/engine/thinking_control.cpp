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

        const std::string prefilled = apply_probe(tmpls, /*enable_thinking=*/false, /*prefill=*/true).prompt;
        if (prefilled == off) return ThinkControl::None; // nothing reaches this model at all

        // The prefill changes the prompt — but that alone does not mean the model will honour it,
        // and the difference is visible in whether the model declares reasoning tags.
        //
        // Tags declared: reasoning is a span the MODEL opens and closes at will. Handing it one that
        // is already closed and empty is a suggestion, and a model not trained on that convention
        // reasons straight past it — measured on LFM2.5-8B-A1B, which ignores the closed span and
        // reasons into the answer instead (issue #82). Worse than leaving it alone, because the
        // reasoning arrives untagged. Report it as uncontrollable rather than making it worse.
        //
        // No tags declared: reasoning is structural — a channel or section the format itself
        // separates — so the prefill does not ask the model to skip anything, it places the model
        // past the reasoning section entirely. That the model cannot ignore (harmony/gpt-oss).
        if (!off_p.thinking_start_tag.empty() || !off_p.thinking_end_tag.empty()) return ThinkControl::None;

        return ThinkControl::Prefill;
    } catch (const std::exception & e) {
        std::fprintf(stderr, "bmoe: thinking-control probe failed (%s); assuming the template honours it\n", e.what());
        return ThinkControl::Template;
    }
}

} // namespace bmoe::detail
