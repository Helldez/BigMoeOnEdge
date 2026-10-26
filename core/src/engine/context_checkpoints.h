#pragma once

// Context checkpoints, the way llama.cpp's server keeps them (tools/server, --ctx-checkpoints).
//
// A chat turn reuses the KV prefix it shares with the last one and removes the rest. Not every
// memory can be cut at an arbitrary position: a recurrent state (Mamba, Gated DeltaNet) holds only
// the latest step, and a sliding-window cache has already dropped what is out of its window. When
// the cut is refused the context has to start again from nothing, and a coding agent's request
// diverges from the cached one on most turns: a client resends the answer as text and the template
// renders it back into tokens that need not be the ones the model sampled. On a hybrid model that
// cost a full re-prefill of a 12k-token prompt on five requests of eight.
//
// So the part of the state that cannot be cut is copied out at points of the prompt, through the
// public state API with LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY (common_prompt_checkpoint, the server's
// own type), and a refused cut rolls back to the newest checkpoint at or before it. Nothing here asks
// what kind of model this is: a memory that cuts anywhere is never asked for a checkpoint's help,
// and has no partial state to copy, so its checkpoints are empty and none is kept.

#include "common.h"
#include "llama.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct common_speculative;

namespace bmoe::detail {

// The contexts a checkpoint covers: the target, and, under MTP speculation, the draft context that
// mirrors its positions plus the speculative driver's own state.
struct CheckpointContexts {
    llama_context * tgt = nullptr;
    llama_context * dft = nullptr;
    common_speculative * spec = nullptr;
};

class ContextCheckpoints {
public:
    // max_count: checkpoints kept, the oldest dropped first (0 = none). min_step: the least spacing
    // between two of them, in tokens, except for the one at the end of a prompt. n_swa: the model's
    // sliding window (llama_model_n_swa), which decides how far back a checkpoint may reach.
    void configure(int max_count, int min_step, int n_swa);

    bool enabled() const { return max_count_ > 0; }
    // Whether a checkpoint after n_tokens is worth taking: the end of a prompt always is, a point
    // inside one only min_step past the last.
    bool wanted(int64_t n_tokens, bool prompt_end) const;
    // Copy the cut-resistant state after the first n_tokens tokens of seq 0. Kept only when there is
    // such a state; returns whether one was kept.
    bool create(const CheckpointContexts & c, int64_t n_tokens);
    // The context refused to drop everything from position n: restore the newest checkpoint that
    // leaves the first `kept` tokens reusable, kept <= n, and drop the context past it. Returns kept,
    // 0 when no checkpoint helps (the caller then clears the context).
    int64_t restore(const CheckpointContexts & c, int64_t n);
    // The context now holds fewer than n tokens of what the checkpoints describe.
    void drop_after(int64_t n);
    void clear() { list_.clear(); }

    size_t count() const { return list_.size(); }
    size_t bytes() const;

private:
    std::vector<common_prompt_checkpoint> list_;
    int max_count_ = 0;
    int min_step_ = 0;
    int n_swa_ = 0;
};

} // namespace bmoe::detail
