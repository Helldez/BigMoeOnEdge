#pragma once

// How a "thinking off" request is actually honoured, decided from the model rather than from a
// table of model names. Kept separate from session.cpp so the decision is unit-testable against
// llama.cpp's bundled templates, with no model and no live session.
//
// Internal header: includes llama.cpp's `common` (not the stable public API), so it must not be
// pulled into core/include/bmoe/. See docs/seam.md.

#include "bmoe/session.h"

#include "chat.h"

namespace bmoe::detail {

// Turn a rendered turn into one that starts with the model's reasoning span already closed.
//
// This sets no markers of its own. It appends a synthetic assistant message and asks for a
// CONTENT continuation, which makes llama.cpp's own per-template handler emit whatever "reasoning
// is over, answer now" looks like for that family — `<think></think>` for the LFM2 and Qwen
// handlers, a primed `<|channel|>final<|message|>` for harmony, and so on. The engine never spells
// any of them out.
//
// The reasoning span carries whitespace, not words: a continuation is only rendered when the
// message is non-empty (`has_continuation()`), and whitespace is exactly what a template that
// implements this natively puts there (Qwen3 renders `<think>\n\n</think>` for enable_thinking
// false). Anything language-bearing would be the engine inventing content for the model.
//
// CONTENT is passed explicitly rather than AUTO: AUTO resolves a reasoning-only message to a
// REASONING continuation, which *opens* the span instead of closing it — the opposite request.
void add_no_think_prefill(common_chat_templates_inputs & inputs);

// Decide which mechanism this model's template supports, by rendering it and looking.
//
// Three renders, once per session:
//   1. enable_thinking true vs false — if the prompt changes, the template reads the flag and
//      nothing further is needed. This is the same render-and-diff llama.cpp's own template
//      analyzer performs; `common_chat_params::supports_thinking` cannot answer the question,
//      because per handler it is a hardcoded literal reporting "this model can reason", not
//      "this template reads the variable".
//   2. with the prefill applied — if that changes the prompt, the handler implements the
//      continuation hook and the span can be closed ahead of generation.
// Neither: the request cannot be honoured on this model, and saying so is better than offering a
// control that does nothing.
//
// Fails open to Template (the pre-existing behaviour: pass the flag and let the template decide),
// because a probe that itself failed is no evidence that the flag is inert.
ThinkControl probe_think_control(const common_chat_templates * tmpls);

} // namespace bmoe::detail
