#pragma once

// How a model honours the "think" request, and the enforcement that makes it true when the
// chat template does not (issue #82).
//
// Setting `enable_thinking = false` only hands a kwarg to the model's Jinja template; whether
// reasoning is actually suppressed is entirely the template's choice. Qwen3-style templates
// render a different prompt and honour it; LFM2.5 never reads the variable, so the request was
// silently discarded and a reasoning model burned its budget on chain-of-thought anyway.
//
// Two separable questions, answered here:
//   * which mechanism can suppress reasoning for THIS model — probe_think_control()
//   * how to actually suppress it when the template will not — make_think_budget_sampler()
//
// Both are derived at runtime from the template's own behaviour and from the tags the loaded
// model reports; no model names, no template string matching, no per-architecture table.
//
// Internal header: includes llama.cpp's `common` (not the stable public API), so it must not
// be pulled into core/include/bmoe/. See docs/seam.md.

#include "bmoe/session.h"

#include "chat.h"
#include "llama.h"

#include <string>
#include <vector>

namespace bmoe::detail {

// Which mechanism, if any, can honour a "no thinking" request for a given chat template.
// Decided once per session by rendering the template both ways and comparing (see the .cpp).
// Fails open to Template: an unknown template is assumed to honour the flag, so a probe
// failure never invents a scary "your toggle does nothing" claim.
ThinkControl probe_think_control(const common_chat_templates * tmpls, const llama_vocab * vocab);

// True when a rendered prompt ends in harmony's assistant marker ("<|start|>assistant", modulo
// trailing whitespace) — the shape gpt-oss uses and no other family does. `trimmed_out`, when
// given, receives the right-trimmed prompt so the caller can append to it without re-trimming.
bool ends_with_harmony_assistant(const std::string & prompt, std::string * trimmed_out = nullptr);

// Build a reasoning-budget sampler bound to THIS turn's rendered template, or nullptr when the
// model exposes no usable thinking tags (fail open — better to reason than to force garbage).
//
// budget: 0 closes the reasoning block the instant the model opens it; N > 0 allows N tokens
// then forces the close. The returned sampler is owned by the caller (llama_sampler_free).
//
// The generation prompt is replayed through the sampler so a template that pre-opens the
// thinking tag in the prompt (rather than letting the model emit it) still arms the counter —
// the tag is never sampled in that case, so without the replay the budget would never engage.
llama_sampler * make_think_budget_sampler(const llama_vocab * vocab, const common_chat_params & cp, int32_t budget);

// When `smpl` is forcing the close of a reasoning block, resolve the token it is pinning and
// return true; otherwise return false and leave `tok` untouched, so the caller falls through to
// its normal sampling path. `scratch` is reused across calls to keep the candidate array off
// the per-token allocation path.
bool think_forced_token(llama_sampler * smpl,
                        const float * logits,
                        int n_vocab,
                        std::vector<llama_token_data> & scratch,
                        llama_token & tok);

} // namespace bmoe::detail
