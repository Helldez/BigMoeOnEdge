#include "thinking_control.h"

#include "common.h"
#include "reasoning-budget.h"

#include <cctype>
#include <cstdio>
#include <exception>

namespace bmoe {

const char * think_control_name(ThinkControl c) {
    switch (c) {
    case ThinkControl::Template:
        return "template";
    case ThinkControl::ForcedFinal:
        return "forced_final";
    case ThinkControl::Sampler:
        return "sampler";
    case ThinkControl::None:
        return "none";
    }
    return "template";
}

} // namespace bmoe

namespace bmoe::detail {

namespace {

// Harmony's generation prompt ends with this and no other family's does, so keying on it
// leaves every other template untouched.
const std::string kHarmonyAsst = "<|start|>assistant";

// Render a one-turn conversation exactly the way generate() renders a real turn — same jinja
// path, same reasoning format — so what the probe observes is what generation will produce.
// The message content is irrelevant: templates branch on the flag, not on the text.
common_chat_params apply_probe(const common_chat_templates * tmpls, bool enable_thinking) {
    common_chat_msg user;
    user.role = "user";
    user.content = "hi";

    common_chat_templates_inputs inputs;
    inputs.messages = {user};
    inputs.add_generation_prompt = true;
    inputs.use_jinja = true;
    inputs.enable_thinking = enable_thinking;
    inputs.reasoning_format = COMMON_REASONING_FORMAT_AUTO;

    return common_chat_templates_apply(const_cast<common_chat_templates *>(tmpls), inputs);
}

// A tag is usable only if it survives tokenization: the sampler matches token sequences, not
// text, so a tag the tokenizer cannot produce would arm nothing.
bool tag_tokenizes(const llama_vocab * vocab, const std::string & tag) {
    if (tag.empty()) {
        return false;
    }
    if (vocab == nullptr) {
        return true; // no vocab to check against (probe without a model): trust the tag
    }
    return !common_tokenize(vocab, tag, /*add_special=*/false, /*parse_special=*/true).empty();
}

} // namespace

bool ends_with_harmony_assistant(const std::string & prompt, std::string * trimmed_out) {
    const size_t last = prompt.find_last_not_of(" \t\r\n");
    const std::string trimmed = (last == std::string::npos) ? prompt : prompt.substr(0, last + 1);
    if (trimmed_out != nullptr) {
        *trimmed_out = trimmed;
    }
    return trimmed.size() >= kHarmonyAsst.size() &&
           trimmed.compare(trimmed.size() - kHarmonyAsst.size(), kHarmonyAsst.size(), kHarmonyAsst) == 0;
}

ThinkControl probe_think_control(const common_chat_templates * tmpls, const llama_vocab * vocab) {
    if (tmpls == nullptr) {
        return ThinkControl::Template;
    }
    try {
        const common_chat_params off = apply_probe(tmpls, /*enable_thinking=*/false);

        // Harmony first: generate() primes the final channel on prompt shape alone, so that
        // mechanism wins regardless of what the flag does to the render. Reporting anything
        // else here would name a mechanism that never runs.
        if (ends_with_harmony_assistant(off.prompt)) {
            return ThinkControl::ForcedFinal;
        }

        const common_chat_params on = apply_probe(tmpls, /*enable_thinking=*/true);

        // The whole question, answered empirically: does the flag change the prompt at all?
        // This is the same render-and-diff llama.cpp's own template analyzer performs. The
        // published `supports_thinking` flag cannot answer it — per-handler it is a hardcoded
        // literal reporting "this model can reason", not "this template reads the variable".
        if (on.prompt != off.prompt) {
            return ThinkControl::Template;
        }

        // The flag is inert. The model can still be silenced if it declares where its
        // reasoning block starts and ends, because that can be enforced on logits.
        if (tag_tokenizes(vocab, off.thinking_start_tag) && tag_tokenizes(vocab, off.thinking_end_tag)) {
            return ThinkControl::Sampler;
        }

        // Flag ignored and no tags to enforce against: the request cannot be honoured. Say so
        // rather than offering a control that does nothing.
        return ThinkControl::None;
    } catch (const std::exception & e) {
        std::fprintf(stderr, "bmoe: thinking-control probe failed (%s); assuming the template honours it\n", e.what());
        return ThinkControl::Template;
    }
}

llama_sampler * make_think_budget_sampler(const llama_vocab * vocab, const common_chat_params & cp, int32_t budget) {
    if (vocab == nullptr || cp.thinking_start_tag.empty() || cp.thinking_end_tag.empty()) {
        return nullptr;
    }

    const std::vector<llama_token> start = common_tokenize(vocab, cp.thinking_start_tag, false, true);
    const std::vector<llama_token> end = common_tokenize(vocab, cp.thinking_end_tag, false, true);
    if (start.empty() || end.empty()) {
        return nullptr;
    }

    // Forced sequence = the closing tag: when the budget runs out the model is made to emit
    // exactly the close it would have written itself, so the reasoning parser still sees a
    // well-formed (empty) block and the answer stays clean.
    llama_sampler * smpl = common_reasoning_budget_init(vocab, start, end, /*forced_tokens=*/end, budget);
    if (smpl == nullptr) {
        return nullptr;
    }

    // Replay whatever the template already placed after the last turn boundary. When a
    // template pre-opens the thinking tag there, the model never samples it, so this is the
    // only chance the counter has to arm.
    if (!cp.generation_prompt.empty()) {
        const std::vector<llama_token> prefill = common_tokenize(vocab, cp.generation_prompt, false, true);
        for (size_t i = 0; i < prefill.size(); ++i) {
            const std::string piece = common_token_to_piece(vocab, prefill[i], true);
            // Some tokenizers prepend a space before a leading special token; feeding it would
            // desync the matcher against what the model actually emits.
            if (i == 0 && !piece.empty() && std::isspace((unsigned char) piece[0]) &&
                !std::isspace((unsigned char) cp.generation_prompt[0])) {
                continue;
            }
            llama_sampler_accept(smpl, prefill[i]);
        }
    }
    return smpl;
}

size_t reasoning_prefix_offset(const std::string & raw, const std::string & start_tag) {
    if (start_tag.empty()) {
        return 0;
    }
    size_t i = 0;
    while (i < raw.size() && std::isspace((unsigned char) raw[i])) {
        ++i;
    }
    // Only when the whitespace is what stands between the parser and the opening tag; leading
    // whitespace in an ordinary answer must be left exactly as the model wrote it.
    if (i > 0 && raw.compare(i, start_tag.size(), start_tag) == 0) {
        return i;
    }
    return 0;
}

bool think_forced_token(llama_sampler * smpl,
                        const float * logits,
                        int n_vocab,
                        std::vector<llama_token_data> & scratch,
                        llama_token & tok) {
    if (smpl == nullptr || logits == nullptr || n_vocab <= 0) {
        return false;
    }
    // Outside FORCING the sampler is a passthrough; skipping it entirely keeps the normal
    // sampling path bit-for-bit what it was before this feature existed.
    if (common_reasoning_budget_get_state(smpl) != REASONING_BUDGET_FORCING) {
        return false;
    }

    scratch.resize((size_t) n_vocab);
    for (int i = 0; i < n_vocab; ++i) {
        scratch[(size_t) i] = llama_token_data{i, logits[i], 0.0f};
    }
    llama_token_data_array cur = {scratch.data(), scratch.size(), -1, false};

    // Drives every candidate but the pinned one to -inf, so the max is the forced token.
    llama_sampler_apply(smpl, &cur);

    size_t best = 0;
    for (size_t i = 1; i < cur.size; ++i) {
        if (cur.data[i].logit > cur.data[best].logit) {
            best = i;
        }
    }
    tok = cur.data[best].id;
    return true;
}

} // namespace bmoe::detail
