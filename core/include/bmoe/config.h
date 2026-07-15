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

    // Size the cache from the device instead of a fixed cache_mb: at init the budget is set to
    // (available RAM − cache_floor_mb), clamped to [cache_min_mb, total expert bytes], and it is
    // re-checked during generation so it shrinks if free memory falls under the floor (and grows
    // back within the floor's headroom). Keeps the phone responsive without a hand-tuned number.
    // Mutually exclusive with an explicit cache_mb > 0. See docs/adaptive-cache.md.
    bool cache_auto = false;
    int cache_floor_mb = 1536; // RAM to leave free for the rest of the system when auto-sizing
    int cache_ceil_mb = 0;     // upper bound on the auto budget in MiB (0 = cap only at the full
                               // expert-set size); useful to keep the cache from taking all the
                               // headroom when the marginal hit-rate gain no longer justifies the RAM

    // Parallel expert-slice read lanes (incl. the calling thread). 1 = serial baseline.
    // Clamped to [1, io_threads_max]. 4 is the measured sweet spot on UFS4 phones.
    int io_threads = 4;

    bool o_direct = true;     // bypass the page cache (O_DIRECT / FILE_FLAG_NO_BUFFERING)
    bool load_all = false;    // debug/A-B: load ALL experts each token (full-sweep baseline)
    bool force_cache = false; // allow a cache_mb in the pathological band (tests/experiments)

    // Overlap async expert reads with FFN compute instead of blocking on them: load_layer()
    // publishes the reads and returns immediately, and the CPU mul_mat_id kernel blocks per
    // expert (via the fork's expert-ready hook) only if that expert's slice is not yet in.
    // Requires the Helldez/llama.cpp fork submodule (the hook); run() fails fast otherwise.
    bool overlap = false;

    // Temporal prefetch: while a token computes layer l, speculatively read on the idle I/O
    // lanes the experts the PREVIOUS token routed at layers l+1..l+prefetch_layers, betting on
    // the strong temporal locality of routing. A correct guess turns the next layer's read into
    // a cache hit; a wrong guess only wastes a read. 0 disables it. Needs the LRU cache on
    // (speculative slices land in the per-layer cache buffers), so validate() rejects it with
    // cache_mb == 0. See docs/prefetch.md.
    int prefetch_layers = 0;

    // Warm the dense (non-expert) file regions into the page cache at init with one sequential
    // buffered sweep: gguf header/metadata, embeddings, attention, norms, lm_head. Those tensors
    // stay mmap-resident (the streamer only rebinds experts), so without warming they demand-page
    // from flash in 4 KiB faults inside the first decodes — on a >RAM model that serialises into
    // tens of seconds of apparent "compute" before the page cache settles. One sweep at load
    // moves that cost into load_seconds at sequential-read bandwidth. Off for A/B measurements.
    bool warm_dense = true;

    // Test/debug only: complete each prefetch's speculative reads synchronously, on the eval
    // thread, before returning. This defeats the latency-hiding purpose (the reads no longer
    // overlap compute) but makes speculative integration deterministic, so the byte-identity
    // gates can exercise the integrate-then-hit path that a timing race otherwise seldom reaches
    // on a fast host. Serial mode only. Never set in production.
    bool prefetch_sync = false;

    // Before a generation, if the kernel has swapped out at least rewarm_threshold_mb of this
    // process's memory, pull the expert cache and the dense regions back in one bulk sequential
    // pass instead of letting the decode fault them in a page at a time.
    //
    // OFF by default: measured on device, it does not work. The pass does what it claims — 2.0 GiB
    // of cache restored, VmSwap 1.76 GiB → 0.46 GiB, in 6.4 s — and the kernel had taken it back
    // 8 seconds later, mid-decode, with the turn still at 0.3 tok/s. Restoring residency cannot win
    // when the process is asking for more than the device will concede (~3.8 GiB wanted against a
    // system that holds MemFree near 100 MiB): it buys seconds of truce for a several-second pause.
    // Kept behind this flag because the mechanism and its telemetry are what a real fix — one that
    // shrinks the ask instead of re-fetching what was taken — will be measured against.
    // See docs/rewarm.md and docs/bench-data/2026-07-15-rewarm/.
    bool rewarm = false;

    // Swapped-out MiB below which a rewarm is not worth its pause. Under a few hundred MiB the
    // refault cost is spread thin enough that the decode absorbs it, while the bulk pass would
    // still stall the first token. 0 means "always rewarm" — how the gates force the pass on a host
    // that has nothing swapped out.
    int rewarm_threshold_mb = 256;

    static constexpr int cache_min_mb = 1500; // smallest non-pathological cache (see above)
    static constexpr int io_threads_max = 8;
    static constexpr int prefetch_layers_max = 8;
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

    // Override the number of active MoE experts per token (top-k routing). 0 = use the
    // model's own <arch>.expert_used_count from the gguf. A lower value cuts per-token
    // compute (and, with moe.enabled, flash I/O) proportionally, at a quality cost — it
    // changes the output. Applied at load via a llama.cpp kv_override on the arch-prefixed
    // expert_used_count metadata key; must stay in [1, n_expert]. Independent of streaming.
    int n_expert_used = 0;

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
