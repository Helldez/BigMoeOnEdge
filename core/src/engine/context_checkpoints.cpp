#include "context_checkpoints.h"

#include "speculative.h"

#include <algorithm>

namespace bmoe::detail {

namespace {

constexpr llama_seq_id k_seq = 0;
constexpr llama_state_seq_flags k_partial = LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY;

bool cut_draft(llama_context * dft, llama_pos p0) {
    return !dft || llama_memory_seq_rm(llama_get_memory(dft), k_seq, p0, -1);
}

} // namespace

void ContextCheckpoints::configure(int max_count, int min_step, int n_swa) {
    max_count_ = std::max(0, max_count);
    min_step_ = std::max(0, min_step);
    n_swa_ = std::max(0, n_swa);
    list_.clear();
}

bool ContextCheckpoints::wanted(int64_t n_tokens, bool prompt_end) const {
    if (!enabled() || n_tokens <= 0) return false;
    if (list_.empty() || prompt_end) return true;
    return n_tokens >= list_.back().n_tokens + min_step_;
}

bool ContextCheckpoints::create(const CheckpointContexts & c, int64_t n_tokens) {
    if (!wanted(n_tokens, /*prompt_end*/ true) || !c.tgt) return false;
    // A memory that cuts anywhere has no partial state: nothing to keep, and nothing it would need.
    if (llama_state_seq_get_size_ext(c.tgt, k_seq, k_partial) == 0) return false;

    // One checkpoint per position: a later prompt that ends where an earlier one did replaces it.
    list_.erase(std::remove_if(list_.begin(), list_.end(),
                               [&](const common_prompt_checkpoint & k) { return k.n_tokens == n_tokens; }),
                list_.end());
    while ((int) list_.size() >= max_count_)
        list_.erase(list_.begin());

    common_prompt_checkpoint & k = list_.emplace_back();
    llama_memory_t mem = llama_get_memory(c.tgt);
    k.update_pos(n_tokens, llama_memory_seq_pos_min(mem, k_seq), llama_memory_seq_pos_max(mem, k_seq));
    k.update_tgt(c.tgt, k_seq, k_partial);
    k.update_dft(c.dft, k_seq, k_partial);
    if (c.spec) common_speculative_get_state(c.spec, k_seq, k.data_spec);
    return true;
}

int64_t ContextCheckpoints::restore(const CheckpointContexts & c, int64_t n) {
    if (!c.tgt) return 0;
    // The first token evaluated after the restore is at position kept, and it reads the positions a
    // sliding window still covers, so the checkpoint's window has to start no later than that; with
    // no window, the checkpoint only has to end before it. The same rule as the server's.
    for (auto it = list_.rbegin(); it != list_.rend(); ++it) {
        const common_prompt_checkpoint & k = *it;
        if (k.n_tokens > n) continue;
        const llama_pos pos_eval = (llama_pos) k.n_tokens;
        const llama_pos thold = std::max<llama_pos>(0, n_swa_ > 0 ? pos_eval - n_swa_ + 2 : pos_eval);
        if (k.pos_max >= pos_eval || !(k.pos_min < thold || k.pos_min == 0)) continue;
        if (!k.load_tgt(c.tgt, k_seq, k_partial) || !k.load_dft(c.dft, k_seq, k_partial)) break;
        if (c.spec) common_speculative_set_state(c.spec, k_seq, k.data_spec);
        // The restored partial state ends at kept; what the rest of the memory holds past it goes.
        if (!llama_memory_seq_rm(llama_get_memory(c.tgt), k_seq, (llama_pos) k.n_tokens, -1)) break;
        if (!cut_draft(c.dft, (llama_pos) k.n_tokens)) break;
        const int64_t kept = k.n_tokens;
        drop_after(kept);
        return kept;
    }
    list_.clear();
    return 0;
}

void ContextCheckpoints::drop_after(int64_t n) {
    list_.erase(
        std::remove_if(list_.begin(), list_.end(), [&](const common_prompt_checkpoint & k) { return k.n_tokens > n; }),
        list_.end());
}

size_t ContextCheckpoints::bytes() const {
    size_t b = 0;
    for (const common_prompt_checkpoint & k : list_)
        b += k.size();
    return b;
}

} // namespace bmoe::detail
