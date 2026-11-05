#include "media_input.h"

#include "bmoe/log.h"

#include "mtmd-helper.h"
#include "mtmd.h"

#include <algorithm>

namespace bmoe {

const char * image_marker() {
    return mtmd_default_marker();
}

} // namespace bmoe

namespace bmoe::detail {

namespace {

// The library logs through ggml's callback; routed to the engine's log so a service's log file
// gets it and a quiet front-end can drop it, as with llama.cpp's own lines.
void mtmd_to_bmoe_log(ggml_log_level level, const char * text, void *) {
    if (!text || !*text) return;
    std::string line(text);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    if (line.empty()) return;
    const LogLevel lv = level == GGML_LOG_LEVEL_ERROR  ? LogLevel::Error
                        : level == GGML_LOG_LEVEL_WARN ? LogLevel::Warn
                                                       : LogLevel::Info;
    log_printf(lv, "projector: %s", line.c_str());
}

} // namespace

std::unique_ptr<MediaInput> MediaInput::open(
    const std::string & mmproj, const llama_model * model, int n_threads, bool use_devices, std::string & err) {
    mtmd_helper_log_set(mtmd_to_bmoe_log, nullptr);
    mtmd_context_params p = mtmd_context_params_default();
    // A device when wanted and the registry has one, the host otherwise: the library asks the
    // registry, so a GPU-less box and a laptop with a GPU run the same code with no branch here.
    p.use_gpu = use_devices;
    p.n_threads = n_threads;
    p.print_timings = false;
    p.warmup = false; // the first image pays the graph's allocation instead of every load paying it
    mtmd_context * c = mtmd_init_from_file(mmproj.c_str(), model, p);
    if (!c) {
        err = "the projector could not be loaded from " + mmproj +
              ": not a projector this model matches (the mmproj file is written by the same conversion as the "
              "model, for the same base model)";
        return nullptr;
    }
    std::unique_ptr<MediaInput> m(new MediaInput());
    m->ctx_ = c;
    m->model_ = model;
    return m;
}

MediaInput::~MediaInput() {
    if (ctx_) mtmd_free(ctx_);
}

bool MediaInput::vision() const {
    return ctx_ && mtmd_support_vision(ctx_);
}

const char * MediaInput::marker() const {
    return ctx_ ? mtmd_get_marker(ctx_) : mtmd_default_marker();
}

MediaInput::Tokenized::~Tokenized() {
    for (mtmd_input_chunk * c : chunks)
        mtmd_input_chunk_free(c);
}

bool MediaInput::tokenize(const std::string & text,
                          const std::vector<ChatImage> & images,
                          bool add_special,
                          Tokenized & out,
                          std::string & err,
                          bool & mismatch) const {
    mismatch = false;
    out.stream.clear();
    for (mtmd_input_chunk * c : out.chunks)
        mtmd_input_chunk_free(c);
    out.chunks.clear();

    std::vector<mtmd_bitmap *> bitmaps;
    bool ok = true;
    for (size_t i = 0; i < images.size() && ok; ++i) {
        const ChatImage & img = images[i];
        mtmd_helper_bitmap_wrapper w = mtmd_helper_bitmap_init_from_buf(
            ctx_, reinterpret_cast<const unsigned char *>(img.bytes.data()), img.bytes.size(),
            /*placeholder*/ false, mtmd_helper_init_opt_default());
        if (w.video_ctx) mtmd_helper_video_free(w.video_ctx); // a still image needs none
        if (!w.bitmap) {
            err = "image " + std::to_string(i + 1) + " could not be decoded" +
                  (img.mime.empty() ? "" : " (" + img.mime + ")") + ": not an image format the projector reads";
            ok = false;
            break;
        }
        if (mtmd_bitmap_is_audio(w.bitmap)) {
            err = "input " + std::to_string(i + 1) + " is audio, and this engine reads images only";
            mtmd_bitmap_free(w.bitmap);
            ok = false;
            break;
        }
        bitmaps.push_back(w.bitmap);
    }

    // The count the library would refuse, checked here first: its reason is a log line, and the
    // request wants the mismatch as its own error.
    if (ok) {
        const std::string mk = marker();
        size_t n_markers = 0;
        for (size_t at = text.find(mk); at != std::string::npos; at = text.find(mk, at + mk.size()))
            ++n_markers;
        if (n_markers != images.size()) {
            mismatch = true;
            err = "the prompt places " + std::to_string(n_markers) + " image marker(s) (" + mk + ") and carries " +
                  std::to_string(images.size()) + " image(s): one marker per image, in order";
            ok = false;
        }
    }
    mtmd_input_chunks * chunks = nullptr;
    if (ok) {
        mtmd_input_text in;
        in.text = text.c_str();
        in.text_len = text.size();
        in.add_special = add_special;
        in.parse_special = true;
        chunks = mtmd_input_chunks_init();
        const int32_t rc = mtmd_tokenize(ctx_, chunks, &in, bitmaps.data(), bitmaps.size());
        if (rc != 0) {
            err = "an image could not be prepared for the projector";
            ok = false;
        }
    }
    if (ok) {
        const size_t n = mtmd_input_chunks_size(chunks);
        for (size_t i = 0; i < n && ok; ++i) {
            const mtmd_input_chunk * c = mtmd_input_chunks_get(chunks, i);
            switch (mtmd_input_chunk_get_type(c)) {
            case MTMD_INPUT_CHUNK_TYPE_TEXT: {
                size_t nt = 0;
                const llama_token * toks = mtmd_input_chunk_get_tokens_text(c, &nt);
                out.stream.tokens.insert(out.stream.tokens.end(), toks, toks + nt);
                break;
            }
            case MTMD_INPUT_CHUNK_TYPE_IMAGE: {
                const char * id = mtmd_input_chunk_get_id(c);
                out.stream.push_media(mtmd_input_chunk_get_n_tokens(c), mtmd_input_chunk_get_n_pos(c), id ? id : "");
                out.chunks.push_back(mtmd_input_chunk_copy(c));
                break;
            }
            default:
                err = "the projector produced an input this engine does not decode (audio)";
                ok = false;
                break;
            }
        }
    }
    if (chunks) mtmd_input_chunks_free(chunks);
    for (mtmd_bitmap * b : bitmaps)
        mtmd_bitmap_free(b);
    if (!ok) {
        out.stream.clear();
        for (mtmd_input_chunk * c : out.chunks)
            mtmd_input_chunk_free(c);
        out.chunks.clear();
    }
    return ok;
}

int MediaInput::decode_media(llama_context * ctx,
                             const Tokenized & t,
                             size_t k,
                             llama_pos pos0,
                             int n_batch,
                             const std::function<int(llama_batch_ext *)> & decode) const {
    if (k >= t.chunks.size() || n_batch <= 0) return -1;
    const mtmd_input_chunk * chunk = t.chunks[k];
    if (mtmd_encode_chunk(ctx_, chunk) != 0) return -1;
    const float * embd = mtmd_get_output_embd(ctx_);
    if (!embd) return -1;

    const int n_tokens = (int) mtmd_input_chunk_get_n_tokens(chunk);
    const size_t n_embd = (size_t) llama_model_n_embd_inp(model_);
    const bool mrope = mtmd_decode_use_mrope(ctx_);
    std::vector<mtmd_decoder_pos> pos2d;
    if (mrope) {
        const mtmd_image_tokens * it = mtmd_input_chunk_get_tokens_image(chunk);
        if (!it) return -1;
        pos2d.resize((size_t) n_tokens);
        mtmd_helper_image_get_decoder_pos(it, pos0, pos2d.data());
    }

    // Some encoders' tokens attend to each other both ways; the library says which, per chunk.
    const bool non_causal = mtmd_decode_use_non_causal(ctx_, chunk);
    if (non_causal) llama_set_causal_attn(ctx, false);
    int rc = 0;
    llama_batch_ext * b = llama_batch_ext_init(ctx);
    for (int off = 0; off < n_tokens && rc == 0; off += n_batch) {
        const int n = std::min(n_batch, n_tokens - off);
        llama_batch_ext_clear(b);
        for (int i = off; i < off + n; ++i) {
            const llama_embd e = {embd + (size_t) i * n_embd, 1, n_embd};
            const int32_t idx = llama_batch_ext_add_embd(b, /*seq*/ 0, e);
            if (idx < 0) {
                rc = -1;
                break;
            }
            llama_pos p[4] = {0, 0, 0, 0};
            if (mrope) {
                p[0] = (llama_pos) pos2d[(size_t) i].t;
                p[1] = (llama_pos) pos2d[(size_t) i].y;
                p[2] = (llama_pos) pos2d[(size_t) i].x;
                p[3] = (llama_pos) pos2d[(size_t) i].z;
            } else {
                p[0] = pos0 + i;
            }
            llama_batch_ext_set_pos(b, idx, p);
            // The last image token keeps its logits: a prompt may end on an image.
            if (i == n_tokens - 1) llama_batch_ext_set_output_logits(b, idx, true);
        }
        if (rc == 0) rc = decode(b);
    }
    llama_batch_ext_free(b);
    if (non_causal) llama_set_causal_attn(ctx, true);
    return rc;
}

} // namespace bmoe::detail
