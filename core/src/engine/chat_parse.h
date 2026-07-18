#pragma once

// Wiring for llama.cpp's reasoning parser. Kept separate from session.cpp so the seam that
// was silently broken (issue #49) is unit-testable without a model or a live session.
//
// Internal header: includes llama.cpp's `common` (not the stable public API), so it must not
// be pulled into core/include/bmoe/. See docs/seam.md.

#include "chat.h"

#include <string>

namespace bmoe::detail {

// Build reasoning-parser params from an applied chat template.
//
// The `common_chat_parser_params(const common_chat_params &)` convenience constructor copies
// only `format` and `generation_prompt` (chat.h) — it leaves the PEG `parser` arena empty, so
// `common_chat_parse` throws on the first token and reasoning markers leak into the answer.
// The serialized grammar has to be loaded explicitly (this is what the upstream server does).
common_chat_parser_params build_parse_params(const common_chat_params & cp);

// Warn exactly once that reasoning parsing failed and the raw stream is being shown instead.
// Returning the raw text is the right degradation, but doing it silently is how #49 stayed
// invisible; the next wiring regression should be noticed.
void warn_parse_failed_once(const char * what);

} // namespace bmoe::detail
