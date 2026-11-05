// The engine's one adapter over llama.cpp's multimodal projector library (tools/mtmd): it opens a
// projector beside the model, turns a prompt's text and images into a PromptStream, and runs an
// image's encoder so its embeddings can be decoded into the context.
//
// Agnostic by construction: which image delimiters the model wants, how an image is tiled, how
// many tokens it takes and which positions they occupy (M-RoPE or not) are all read from the
// projector file by the library; nothing here names a model family or a format. The session owns
// the decode itself (placement, the arena, the route trace), so this type hands back batches and
// never calls llama_decode.
#pragma once

#include "bmoe/chat.h"
#include "prompt_stream.h"

#include "llama.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct mtmd_context;
struct mtmd_input_chunk;

namespace bmoe::detail {

class MediaInput {
public:
    // Open the projector `mmproj` for `model`. With `use_devices` the encoder and its weights go on
    // a device of the backends loaded, when one is registered (the library's choice, from the
    // registry); without, on the host. The caller decides by the rule its model follows.
    static std::unique_ptr<MediaInput>
    open(const std::string & mmproj, const llama_model * model, int n_threads, bool use_devices, std::string & err);
    ~MediaInput();

    bool vision() const;
    // The marker a prompt places an image with: the library's, the same string image_marker() gives.
    const char * marker() const;

    // A prompt tokenized with its images. The spans in `stream` are the chunks in `chunks`, in order.
    struct Tokenized {
        PromptStream stream;
        std::vector<mtmd_input_chunk *> chunks; // owned; freed with this object
        Tokenized() = default;
        Tokenized(const Tokenized &) = delete;
        Tokenized & operator=(const Tokenized &) = delete;
        ~Tokenized();
    };
    // Tokenize `text` with `images` placed at its markers. False with `err` when the marker count
    // and the image count differ (`mismatch` set), or an image could not be read.
    bool tokenize(const std::string & text,
                  const std::vector<ChatImage> & images,
                  bool add_special,
                  Tokenized & out,
                  std::string & err,
                  bool & mismatch) const;

    // Encode span k of `t` and hand its embeddings to `decode`, n_batch tokens at a time, positioned
    // from pos0 (the span's own 2-D positions under M-RoPE, pos0 + i otherwise). The model's
    // attention mode is switched for the span where its projector asks (some encoders are read with
    // non-causal attention) and restored after. Returns 0, or the first non-zero `decode` result, or
    // -1 when the encoder failed.
    int decode_media(llama_context * ctx,
                     const Tokenized & t,
                     size_t k,
                     llama_pos pos0,
                     int n_batch,
                     const std::function<int(llama_batch_ext *)> & decode) const;

private:
    MediaInput() = default;
    mtmd_context * ctx_ = nullptr;
    const llama_model * model_ = nullptr;
};

} // namespace bmoe::detail
