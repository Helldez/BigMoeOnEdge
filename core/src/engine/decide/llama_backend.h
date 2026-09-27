#pragma once

// IDecideBackend over a live session's llama.cpp context. The only file of decide/ that includes
// llama.cpp; it is handed exactly the pieces of the session it touches, not the session itself.

#include "backend.h"
#include "../prefill_support.h"

#include "bmoe/expert_source.h"
#include "bmoe/session.h"

#include "chat.h"
#include "llama.h"

#include <type_traits>

namespace bmoe {
class RouterHook;
}

namespace bmoe::detail {

static_assert(std::is_same<Token, llama_token>::value, "decide's Token must be llama_token");

struct LlamaDecideDeps {
    llama_context * ctx = nullptr;
    llama_context * ctx_dft = nullptr; // the MTP draft context, emptied alongside; null without one
    const llama_vocab * vocab = nullptr;
    int n_vocab = 0;
    int n_ctx = 0;
    int n_batch = 0;
    RouterHook * hook = nullptr;
    const IExpertSource * source = nullptr; // measured only when moe_on
    bool moe_on = false;
    const common_chat_templates * tmpls = nullptr; // null when chat mode is off: the prompt is raw
    ThinkControl think_ctl = ThinkControl::Template;
};

class LlamaDecideBackend final : public IDecideBackend {
public:
    explicit LlamaDecideBackend(const LlamaDecideDeps & d);
    ~LlamaDecideBackend() override;

    bool render(const std::string & content, bool think, std::vector<Token> & out, std::string & error) override;
    std::vector<Token> tokenize_plain(const std::string & text) override;
    int n_ctx() const override { return d_.n_ctx; }
    int n_vocab() const override { return d_.n_vocab; }
    // The CPU prefill this session runs costs in proportion to the tokens it is fed.
    bool prefill_cost_scales_with_tokens() const override { return true; }
    void clear() override;
    bool prefill(const std::vector<Token> & tokens, int from, int to) override;
    const float * last_logits() override;
    bool save_state(std::vector<uint8_t> & out) override;
    bool load_state(const std::vector<uint8_t> & in) override;
    void begin_prefill_measure() override;
    void end_prefill_measure(PrefillStats & out) override;

private:
    std::vector<Token> tokenize(const std::string & text, bool special) const;

    LlamaDecideDeps d_;
    llama_batch batch_;
    bool have_logits_ = false;
    PrefillTally tally_;
};

} // namespace bmoe::detail
