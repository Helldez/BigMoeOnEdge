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
        const std::string off = apply_probe(tmpls, /*enable_thinking=*/false, /*prefill=*/false).prompt;
        const std::string on = apply_probe(tmpls, /*enable_thinking=*/true, /*prefill=*/false).prompt;
        if (on != off) return ThinkControl::Template;

        const std::string prefilled = apply_probe(tmpls, /*enable_thinking=*/false, /*prefill=*/true).prompt;
        if (prefilled != off) return ThinkControl::Prefill;

        return ThinkControl::None;
    } catch (const std::exception & e) {
        std::fprintf(stderr, "bmoe: thinking-control probe failed (%s); assuming the template honours it\n", e.what());
        return ThinkControl::Template;
    }
}

} // namespace bmoe::detail
