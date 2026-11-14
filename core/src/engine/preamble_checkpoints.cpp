#include "preamble_checkpoints.h"

#include <algorithm>
#include <string>

namespace bmoe::detail {

namespace {

constexpr llama_seq_id k_seq = 0;
constexpr llama_state_seq_flags k_whole = 0;

std::vector<llama_token> tokenize_text(const llama_vocab * vocab, const std::string & s) {
    // Room for the specials the template's text holds; a buffer that is too short only costs a second call.
    std::vector<llama_token> t(s.size() + 8);
    int n = llama_tokenize(vocab, s.c_str(), (int) s.size(), t.data(), (int) t.size(), true, true);
    if (n < 0) {
        t.resize((size_t) -n);
        n = llama_tokenize(vocab, s.c_str(), (int) s.size(), t.data(), (int) t.size(), true, true);
    }
    t.resize(n > 0 ? (size_t) n : 0);
    return t;
}

} // namespace

PreambleProbe probe_preamble(const common_chat_templates * tmpls,
                             const common_chat_templates_inputs & inputs,
                             const llama_vocab * vocab) {
    PreambleProbe p;
    const std::vector<common_chat_msg> & msgs = inputs.messages;
    if (!tmpls || msgs.empty()) return p;
    if (inputs.tools.empty() && msgs.front().role != "system") return p;
    size_t first_user = 0;
    while (first_user < msgs.size() && msgs[first_user].role != "user")
        ++first_user;
    if (first_user >= msgs.size()) return p;

    common_chat_templates_inputs probe_in = inputs;
    probe_in.add_generation_prompt = false;
    probe_in.messages.assign(msgs.begin(), msgs.begin() + (std::ptrdiff_t) first_user + 1);
    common_chat_msg & stand_in = probe_in.messages.back();
    stand_in.content_parts.clear();
    try {
        stand_in.content = "A";
        p.a = tokenize_text(vocab, common_chat_templates_apply(tmpls, probe_in).prompt.str());
        stand_in.content = "\nB";
        p.b = tokenize_text(vocab, common_chat_templates_apply(tmpls, probe_in).prompt.str());
    } catch (const std::exception &) {
        // A template that cannot render the stand-in: no boundary, and so no checkpoint.
        p.a.clear();
        p.b.clear();
    }
    return p;
}

size_t preamble_end(const PreambleProbe & probe, const std::vector<llama_token> & prompt) {
    const size_t lim = std::min({probe.a.size(), probe.b.size(), prompt.size()});
    size_t n = 0;
    while (n < lim && probe.a[n] == probe.b[n] && probe.a[n] == prompt[n])
        ++n;
    return n >= k_preamble_checkpoint_min_tokens && n < prompt.size() ? n : 0;
}

void PreambleCheckpoints::configure(int max_count, int64_t max_tokens) {
    max_count_ = std::max(0, max_count);
    max_tokens_ = std::max<int64_t>(0, max_tokens);
    slots_.clear();
}

std::vector<PreambleCheckpoints::Slot>::const_iterator PreambleCheckpoints::find(const llama_token * toks,
                                                                                 size_t n) const {
    return std::find_if(slots_.begin(), slots_.end(), [&](const Slot & s) {
        return s.tokens.size() == n && std::equal(s.tokens.begin(), s.tokens.end(), toks);
    });
}

bool PreambleCheckpoints::wanted(const llama_token * toks, size_t n) const {
    if (!enabled() || n == 0 || (int64_t) n > max_tokens_) return false;
    return find(toks, n) == slots_.end();
}

bool PreambleCheckpoints::create(const CheckpointContexts & c, const llama_token * toks, size_t n) {
    if (!wanted(toks, n) || !c.tgt) return false;
    Slot s;
    s.state.resize(llama_state_seq_get_size_ext(c.tgt, k_seq, k_whole));
    if (s.state.empty() ||
        llama_state_seq_get_data_ext(c.tgt, s.state.data(), s.state.size(), k_seq, k_whole) != s.state.size())
        return false;
    s.tokens.assign(toks, toks + n);
    // Room first, in count and in tokens, so the bound holds at every moment.
    while (!slots_.empty() && ((int) slots_.size() >= max_count_ || tokens() + (int64_t) n > max_tokens_))
        slots_.erase(slots_.begin());
    slots_.push_back(std::move(s));
    return true;
}

size_t PreambleCheckpoints::match(const llama_token * toks, size_t n, size_t held) const {
    size_t best = 0;
    for (const Slot & s : slots_) {
        const size_t len = s.tokens.size();
        if (len <= held || len <= best || len > n) continue;
        if (std::equal(s.tokens.begin(), s.tokens.end(), toks)) best = len;
    }
    return best;
}

bool PreambleCheckpoints::restore(const CheckpointContexts & c, const llama_token * toks, size_t n) {
    const auto it = find(toks, n);
    if (it == slots_.end() || !c.tgt) return false;
    const size_t at = (size_t) (it - slots_.begin());
    const Slot & s = slots_[at];
    const bool loaded =
        llama_state_seq_set_data_ext(c.tgt, s.state.data(), s.state.size(), k_seq, k_whole) == s.state.size() &&
        llama_memory_seq_pos_max(llama_get_memory(c.tgt), k_seq) + 1 == (llama_pos) n;
    if (!loaded) {
        slots_.erase(slots_.begin() + (std::ptrdiff_t) at);
        return false;
    }
    std::rotate(slots_.begin() + (std::ptrdiff_t) at, slots_.begin() + (std::ptrdiff_t) at + 1, slots_.end());
    return true;
}

size_t PreambleCheckpoints::bytes() const {
    size_t b = 0;
    for (const Slot & s : slots_)
        b += s.state.size();
    return b;
}

int64_t PreambleCheckpoints::tokens() const {
    int64_t t = 0;
    for (const Slot & s : slots_)
        t += (int64_t) s.tokens.size();
    return t;
}

} // namespace bmoe::detail
