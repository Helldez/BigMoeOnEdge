#include "gguf_offsets.h"

#include "gguf.h"

namespace bmoe {

GgufOffsets read_gguf_offsets(const char * path) {
    GgufOffsets out;

    gguf_init_params gp{};
    gp.no_alloc = true; // metadata + offsets only; no tensor bytes touched
    gp.ctx = nullptr;

    gguf_context * gctx = gguf_init_from_file(path, gp);
    if (!gctx) {
        return out;
    }

    const uint64_t data_off = (uint64_t) gguf_get_data_offset(gctx);
    const int64_t n = gguf_get_n_tensors(gctx);
    out.off_by_name.reserve((size_t) n);
    for (int64_t i = 0; i < n; ++i) {
        out.off_by_name[gguf_get_tensor_name(gctx, i)] = data_off + (uint64_t) gguf_get_tensor_offset(gctx, i);
    }

    gguf_free(gctx);
    out.ok = true;
    return out;
}

} // namespace bmoe
