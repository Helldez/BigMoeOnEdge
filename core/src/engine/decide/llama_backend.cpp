#include "llama_backend.h"

#include "../thinking_control.h"
#include "../../moe/router_hook.h"

#include <algorithm>
#include <exception>

namespace bmoe::detail {

LlamaDecideBackend::LlamaDecideBackend(const LlamaDecideDeps & d)
    : d_(d), batch_(llama_batch_init(d.n_batch, /*embd*/ 0, /*n_seq_max*/ 1)) {}

LlamaDecideBackend::~LlamaDecideBackend() {
    llama_batch_free(batch_);
}

std::vector<Token> LlamaDecideBackend::tokenize(const std::string & text, bool special) const {
    std::vector<Token> t(text.size() + 8);
    int n = llama_tokenize(d_.vocab, text.c_str(), (int) text.size(), t.data(), (int) t.size(), special, special);
    if (n < 0) {
        t.resize(-n);
        n = llama_tokenize(d_.vocab, text.c_str(), (int) text.size(), t.data(), (int) t.size(), special, special);
    }
    t.resize(std::max(0, n));
    return t;
}

bool LlamaDecideBackend::render(const std::string & content, std::vector<Token> & out, std::string & error) {
    if (!d_.tmpls) {
        out = tokenize(content, true);
        return true;
    }
    try {
        common_chat_msg msg;
        msg.role = "user";
        msg.content = content;
        common_chat_templates_inputs inputs;
        build_turn_inputs(inputs, {msg}, /*think*/ false, d_.think_ctl);
        out = tokenize(common_chat_templates_apply(d_.tmpls, inputs).prompt, true);
        return true;
    } catch (const std::exception & e) {
        error = std::string("chat template apply failed: ") + e.what();
        return false;
    }
}

std::vector<Token> LlamaDecideBackend::tokenize_plain(const std::string & text) {
    return tokenize(text, false);
}

void LlamaDecideBackend::clear() {
    llama_memory_clear(llama_get_memory(d_.ctx), true);
    if (d_.ctx_dft) llama_memory_clear(llama_get_memory(d_.ctx_dft), true);
    have_logits_ = false;
}

bool LlamaDecideBackend::prefill(const std::vector<Token> & tokens, int from, int to) {
    const int n = (int) tokens.size();
    have_logits_ = false;
    for (int i = from; i < to; i += d_.n_batch) {
        const int chunk = std::min(d_.n_batch, to - i);
        // Logits only on the prompt's last position: a decision reads one distribution, and asking
        // for more makes every chunk pay for vocab-wide output rows nobody reads.
        batch_fill(batch_, tokens.data() + i, chunk, /*pos0*/ i, /*all_logits*/ false);
        if (i + chunk < n) batch_.logits[chunk - 1] = 0;
        // Prefill phase: the decode-only routing policies stay out of it, as they do in generate().
        d_.hook->set_batch_phase(0);
        if (llama_decode(d_.ctx, batch_) != 0) return false;
    }
    have_logits_ = to == n && to > from;
    return true;
}

const float * LlamaDecideBackend::last_logits() {
    return have_logits_ ? llama_get_logits_ith(d_.ctx, -1) : nullptr;
}

bool LlamaDecideBackend::save_state(std::vector<uint8_t> & out) {
    out.resize(llama_state_seq_get_size(d_.ctx, 0));
    const size_t wrote = llama_state_seq_get_data(d_.ctx, out.data(), out.size(), 0);
    out.resize(wrote);
    return wrote > 0;
}

bool LlamaDecideBackend::load_state(const std::vector<uint8_t> & in) {
    return !in.empty() && llama_state_seq_set_data(d_.ctx, in.data(), in.size(), 0) != 0;
}

void LlamaDecideBackend::begin_prefill_measure() {
    tally_.begin(d_.moe_on, *d_.source);
}

void LlamaDecideBackend::end_prefill_measure(PrefillStats & out) {
    tally_.end(d_.moe_on, *d_.source);
    out.cpu_seconds = tally_.cpu_seconds;
    out.read_mib = tally_.read_mib;
    out.io_seconds = tally_.io_seconds;
    out.stall_seconds = tally_.stall_seconds;
    out.mgmt_seconds = tally_.mgmt_seconds;
}

} // namespace bmoe::detail
