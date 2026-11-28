// Fills llama.cpp's device expert cache in place from the model file.
//
// Where the host writes the device's memory directly (unified memory: the backend exports
// ggml_backend_buffer_get_host_base), a miss needs no host copy and no upload: the expert's bytes
// are read from the file straight into the cache's slot. A pool of reader lanes does it, one read
// per expert projection, at the offsets the file declares (gguf_offsets). llama.cpp hands the slots
// over through llama_set_moe_cache_fill and says when the device needs them (a synchronous fill)
// and when they may land later (read-ahead).
#pragma once

#include <memory>
#include <string>

struct llama_context;

namespace bmoe {

class CacheFill {
public:
    // Opens the model's shards on `lanes` reader lanes and installs the fill on ctx. nullptr where the
    // file cannot be read this way or llama.cpp refuses (no cache, or memory the host cannot write).
    // async_delay_us > 0 (debug) holds every background fill that long before it reads (see
    // MoeStreamConfig::device_cache_fill_delay_us).
    static std::unique_ptr<CacheFill>
    install(llama_context * ctx, const std::string & model_path, int lanes, int async_delay_us = 0);
    ~CacheFill();

    CacheFill(const CacheFill &) = delete;
    CacheFill & operator=(const CacheFill &) = delete;

    struct Impl;

private:
    explicit CacheFill(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace bmoe
