#include "draft_driver.h"

#include <algorithm>
#include <vector>

namespace bmoe::detail {

std::string DraftKind::name() const {
    return common_speculative_type_to_str(type);
}

DraftKind draft_kind(const SpecConfig & spec) {
    if (!spec.uses_driver()) return {};
    // The head's source says what drafts: the model's head, in the model or in the file it names.
    // Whether that file holds one is checked on the loaded model (Session::open), as it always was.
    if (spec.is_mtp()) return {COMMON_SPECULATIVE_TYPE_DRAFT_MTP};
    const std::vector<common_speculative_type> types = common_speculative_types_from_gguf(spec.draft_file());
    return types.empty() ? DraftKind{} : DraftKind{types.front()};
}

llama_model_params
draft_file_model_params(const DraftKind & kind, const SpecConfig & spec, ggml_backend_dev_t * devices) {
    llama_model_params p = llama_model_default_params();
    if (!spec.draft_file_on_devices()) p.n_gpu_layers = 0;
    p.devices = devices;
    p.load_mtp = kind.is_head();
    return p;
}

llama_context_params
draft_context_params(const DraftKind & kind, const SpecConfig & spec, const llama_context_params & target) {
    llama_context_params d = target;
    if (kind.is_head()) d.ctx_type = LLAMA_CONTEXT_TYPE_MTP;
    // One device cache, the target's: a draft's own expert layer is served from the host, and a
    // second cache would take as much device memory again for a graph that routes one position.
    d.moe_cache_size = 0;
    // Nothing is ever rolled back on the draft side.
    d.n_rs_seq = 0;
    // The target's output cap is about the target's logits. The head keeps llama.cpp's own, which
    // its narrow ubatch below already bounds: it drafts a position per decode. A draft model may
    // emit its whole draft in one decode and has it sampled on the backend, which llama.cpp only
    // does up to the context's per-sequence cap, so it gets the cap the driver's own sizing rule
    // gives one sequence drafting draft_max tokens.
    d.n_outputs_max = 0;
    if (!kind.is_head()) {
        const common_speculative_output_limits lim =
            common_speculative_get_output_limits((int32_t) target.n_batch, /*n_parallel*/ 1, spec.draft_max);
        d.n_outputs_max = (uint32_t) lim.total;
        d.n_outputs_max_per_seq = (uint32_t) lim.per_seq;
    }
    // Compute buffers are reserved for the WIDEST ubatch, and left at the target's width the draft
    // context reserves gigabytes for a graph it never runs: the widest thing it ever computes is a
    // prefill chunk, and at decode time it is 1 + draft_max positions. On this engine every MiB
    // reserved is a MiB the expert cache does not get, and the cache is what decides whether the
    // widened verify read set is a hit or a flash read - so an oversized draft context does not
    // merely waste memory, it eats the feature's own prize. Measured on the desktop host with the
    // model's head: leaving it at the target's width cost ~1 GiB of expert cache and 3 points of hit
    // rate. n_batch stays as it is - the driver sizes its internal batch from it and must still
    // accept a whole prefill chunk, which llama.cpp then splits into ubatches of the width below.
    d.n_ubatch =
        std::min<uint32_t>(target.n_ubatch, (uint32_t) std::max(spec.draft_max + 1, SpecConfig::draft_context_ubatch));
    return d;
}

common_speculative *
make_draft_driver(const DraftKind & kind, const SpecConfig & spec, llama_context * ctx_tgt, llama_context * ctx_dft) {
    common_params_speculative sp;
    sp.types = {kind.type};
    sp.draft.n_max = spec.draft_max;
    sp.draft.n_min = 0; // never skip a whole draft; width is bounded by n_max and p_min below
    // The draft's own confidence floor for continuing. At 0 (the default) it drafts n_max tokens
    // however unsure it is, which is what the host was measured at; above 0 the width becomes
    // adaptive per step. See SpecConfig::draft_p_min for why that pays twice on a streamed device.
    sp.draft.p_min = spec.draft_p_min;
    sp.draft.ctx_tgt = ctx_tgt; // two contexts: over one model, or over the model and its draft file
    sp.draft.ctx_dft = ctx_dft;
    return common_speculative_init(sp, /*n_seq*/ 1);
}

} // namespace bmoe::detail
