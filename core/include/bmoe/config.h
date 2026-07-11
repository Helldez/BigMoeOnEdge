// Engine configuration.
//
// All tunables flow through these structs: the CLI parses flags into a RunConfig, the
// engine consumes it. The library never reads environment variables — env overrides,
// if any, are resolved in the CLI before this struct is built (see cli/main.cpp), so
// the engine's behaviour is fully determined by its inputs and is trivially testable.
//
// This header is pure policy (no llama.cpp dependency); validate() is unit-tested
// without the native backend.
#pragma once

#include <string>

namespace bmoe {

// MoE expert-selective streaming knobs.
struct MoeStreamConfig {
    bool enabled = false; // turn streaming on; init fails fast if the model is not MoE

    // LRU expert cache budget, in MiB. 0 disables the cache (experts are re-read from
    // flash every token via three shared slots). Measured pathology: a budget below one
    // token's working set thrashes (evict + re-read, zero hits) and is SLOWER than off,
    // so validate() rejects the 1..cache_min_mb-1 band unless force_cache is set.
    int cache_mb = 0;

    // Parallel expert-slice read lanes (incl. the calling thread). 1 = serial baseline.
    // Clamped to [1, io_threads_max]. 4 is the measured sweet spot on UFS4 phones.
    int io_threads = 4;

    bool o_direct = true;     // bypass the page cache (O_DIRECT / FILE_FLAG_NO_BUFFERING)
    bool load_all = false;    // debug/A-B: load ALL experts each token (full-sweep baseline)
    bool force_cache = false; // allow a cache_mb in the pathological band (tests/experiments)

    static constexpr int cache_min_mb = 1500; // smallest non-pathological cache (see above)
    static constexpr int io_threads_max = 8;
};

// A full run: model, prompt, decoding, streaming, telemetry.
struct RunConfig {
    std::string model_path;
    std::string prompt = "The capital of Japan is";
    int n_predict = 32;
    int n_threads = 4;
    int n_ctx = 2048;
    bool chatml = false;   // wrap the prompt in the model family's chat turn (arch-aware)
    bool progress = false; // emit machine telemetry (one JSON line per token)

    // Render the chat template with reasoning enabled. Passed to the template as the
    // `enable_thinking` kwarg, so a reasoning model (Qwen3, thinking Gemma, …) only emits
    // its thinking channel when true. Off suppresses reasoning at the source rather than
    // relying on the display-time parser, which cannot strip a format it does not know.
    // Only meaningful with chatml; the raw-prompt path ignores it.
    bool think = true;

    MoeStreamConfig moe;
};

// Validation result: ok plus a human-readable reason when not.
struct ValidationResult {
    bool ok = true;
    std::string error;
    explicit operator bool() const { return ok; }
};

// Check a RunConfig for internal consistency. Enforces, among others: MoE streaming
// requires a model path; cache_mb is 0 or >= cache_min_mb (unless force_cache);
// io_threads in range; n_predict/n_threads positive. Pure function — no I/O.
ValidationResult validate(const RunConfig & cfg);

} // namespace bmoe
