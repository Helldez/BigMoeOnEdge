#pragma once

// Preamble checkpoints: the whole state at the end of a conversation's preamble, kept by its tokens.
//
// An agent client opens every conversation with the same preamble, a system prompt and its tools,
// thousands of tokens long, and it opens conversations all the time (sub-tasks, new sessions). On a
// memory that cannot be cut anywhere, a new conversation reuses that prefix only from a context
// checkpoint at or before its end (context_checkpoints.h), and none is kept there: one sits where
// each prompt ended, inside the first conversation, and the ring drops the oldest first. So the
// preamble was prefilled again, which on a CPU is the longest wait of a session.
//
// A context checkpoint at the preamble's end, held apart from the ring, would serve the next
// conversation but not the one after another preamble: it copies only the part of the state that
// cannot be cut, and the rest is in the context, which the other preamble has replaced. Two agents
// taking turns, or one client that sends a different tool list now and then, would push each other
// out on every switch. So what is kept here is the whole state of the prefix
// (llama_state_seq_get_data_ext without LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY) together with its
// tokens: it stands without the live context, and a prompt that starts with those tokens restores it
// whatever the context held before. A few are kept, the least recently used dropped first.
//
// Only a memory that cannot be cut anywhere needs them, and the session configures none elsewhere:
// a context that is all attention cuts back to a shared prefix by itself, and the whole state of
// that prefix would cost its attention cache a second time.
//
// What they hold is bounded twice, in count and in tokens together, and the plan charges exactly
// that bound (planner.cpp, checkpoint_reserve), so the memory a run takes is the memory it planned.

#include "chat.h"
#include "context_checkpoints.h"
#include "llama.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bmoe::detail {

// A preamble shorter than this is not worth a checkpoint: prefilling it again is one small piece,
// and its state would still copy the whole fixed part of the context (62.8 MiB on Qwen3.6-35B-A3B).
inline constexpr size_t k_preamble_checkpoint_min_tokens = 256;

// Where a conversation's preamble ends, found without knowing the template: the conversation up to
// its first user message is rendered twice, with contents that differ in their first character, one
// led by a newline so that a message starting with whitespace still shares the prefix, and the
// preamble is the longest token prefix both renders and the real prompt share. Empty when there is
// no preamble to share (no system message and no tools) or the template cannot render the stand-in.
struct PreambleProbe {
    std::vector<llama_token> a, b;
};
// `inputs` is the real render's, after the reasoning switches and the tools are set, so the probe
// renders see what it saw. Two renders of the preamble: milliseconds against its prefill.
PreambleProbe probe_preamble(const common_chat_templates * tmpls,
                             const common_chat_templates_inputs & inputs,
                             const llama_vocab * vocab);
// The preamble's length in tokens of `prompt`, 0 when there is none of at least
// k_preamble_checkpoint_min_tokens or nothing would be left to decode after it. An image is not
// part of a preamble: its placeholders are no vocabulary tokens, so the first one ends the prefix.
size_t preamble_end(const PreambleProbe & probe, const std::vector<llama_token> & prompt);

class PreambleCheckpoints {
public:
    // max_count: preambles kept, the least recently used dropped first (0 = none). max_tokens: what
    // they may hold together, in tokens; a preamble longer than that is not kept.
    void configure(int max_count, int64_t max_tokens);
    bool enabled() const { return max_count_ > 0 && max_tokens_ > 0; }

    // Whether the state after the first n tokens of toks is worth taking: within the bounds, and not
    // held already.
    bool wanted(const llama_token * toks, size_t n) const;
    // Copy the whole state of seq 0, which holds exactly the first n tokens of toks. Only the target
    // context is read: the session keeps none where a second context mirrors it. Returns whether one
    // was kept.
    bool create(const CheckpointContexts & c, const llama_token * toks, size_t n);
    // The longest kept preamble the first n tokens of toks start with, when it is longer than the
    // `held` tokens the context still holds of them: its length, 0 when none helps.
    size_t match(const llama_token * toks, size_t n, size_t held) const;
    // Load the kept state of the first n tokens of toks (n from match) into seq 0, which the caller
    // has cleared. True when the context now holds exactly those tokens. A state that does not load
    // is dropped, and the caller clears the context again.
    bool restore(const CheckpointContexts & c, const llama_token * toks, size_t n);
    void clear() { slots_.clear(); }

    size_t count() const { return slots_.size(); }
    size_t bytes() const;
    int64_t tokens() const;

private:
    struct Slot {
        std::vector<llama_token> tokens;
        std::vector<uint8_t> state;
    };
    std::vector<Slot>::const_iterator find(const llama_token * toks, size_t n) const;
    std::vector<Slot> slots_; // the most recently used last
    int max_count_ = 0;
    int64_t max_tokens_ = 0;
};

} // namespace bmoe::detail
