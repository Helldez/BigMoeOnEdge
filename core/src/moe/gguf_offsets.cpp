#include "gguf_offsets.h"

#include "gguf.h"

#include <string>

namespace bmoe {

namespace {

// Read a gguf metadata value as an int, accepting any of the integer scalar types the
// writer might have used for a count (llama.cpp writes expert counts as UINT32, but be
// permissive). Returns `dflt` if the key is absent or not an integer scalar.
int meta_int(const gguf_context * ctx, const std::string & key, int dflt) {
    const int64_t id = gguf_find_key(ctx, key.c_str());
    if (id < 0) return dflt;
    switch (gguf_get_kv_type(ctx, id)) {
    case GGUF_TYPE_UINT8:
        return (int) gguf_get_val_u8(ctx, id);
    case GGUF_TYPE_INT8:
        return (int) gguf_get_val_i8(ctx, id);
    case GGUF_TYPE_UINT16:
        return (int) gguf_get_val_u16(ctx, id);
    case GGUF_TYPE_INT16:
        return (int) gguf_get_val_i16(ctx, id);
    case GGUF_TYPE_UINT32:
        return (int) gguf_get_val_u32(ctx, id);
    case GGUF_TYPE_INT32:
        return (int) gguf_get_val_i32(ctx, id);
    case GGUF_TYPE_UINT64:
        return (int) gguf_get_val_u64(ctx, id);
    case GGUF_TYPE_INT64:
        return (int) gguf_get_val_i64(ctx, id);
    default:
        return dflt;
    }
}

} // namespace

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

GgufModelInfo read_gguf_model_info(const char * path) {
    GgufModelInfo out;

    gguf_init_params gp{};
    gp.no_alloc = true; // metadata only; no tensor bytes touched
    gp.ctx = nullptr;

    gguf_context * gctx = gguf_init_from_file(path, gp);
    if (!gctx) {
        return out;
    }

    const int64_t arch_id = gguf_find_key(gctx, "general.architecture");
    if (arch_id >= 0 && gguf_get_kv_type(gctx, arch_id) == GGUF_TYPE_STRING) {
        out.arch = gguf_get_val_str(gctx, arch_id);
    }
    if (!out.arch.empty()) {
        // Arch-prefixed keys, exactly as llama.cpp names them (LLM_KV_EXPERT_COUNT /
        // LLM_KV_EXPERT_USED_COUNT expand "%s" to the architecture).
        out.n_expert = meta_int(gctx, out.arch + ".expert_count", 0);
        out.n_expert_used = meta_int(gctx, out.arch + ".expert_used_count", 0);
    }

    gguf_free(gctx);
    out.ok = true;
    return out;
}

} // namespace bmoe
