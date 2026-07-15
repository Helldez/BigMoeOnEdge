#include "bmoe/config.h"

#include <utility>

namespace bmoe {

ValidationResult validate(const RunConfig & cfg) {
    ValidationResult r;
    auto fail = [&](std::string msg) {
        r.ok = false;
        r.error = std::move(msg);
        return r;
    };

    if (cfg.model_path.empty()) {
        return fail("model_path is required");
    }
    if (cfg.n_predict <= 0) {
        return fail("n_predict must be positive");
    }
    if (cfg.n_threads <= 0) {
        return fail("n_threads must be positive");
    }
    if (cfg.n_ctx <= 0) {
        return fail("n_ctx must be positive");
    }
    // Lower bound only: 0 means "use the model default". The upper bound (<= the model's
    // real expert count) needs the loaded gguf, so it is deferred to run() where the model
    // is available — same rationale as the streaming checks that stay out of this pure path.
    if (cfg.n_expert_used < 0) {
        return fail("n_expert_used must be >= 0 (0 = model default)");
    }

    // overlap is meaningless without streaming (it gates the streamer's own reads). The
    // hook-availability check is deferred to run(): validate() stays pure (no native).
    if (cfg.moe.overlap && !cfg.moe.enabled) {
        return fail("moe.overlap requires moe.enabled");
    }

    if (cfg.moe.enabled) {
        const MoeStreamConfig & m = cfg.moe;
        if (m.io_threads < 1 || m.io_threads > MoeStreamConfig::io_threads_max) {
            return fail("moe.io_threads must be in [1, " + std::to_string(MoeStreamConfig::io_threads_max) + "]");
        }
        if (m.cache_mb < 0) {
            return fail("moe.cache_mb must be >= 0");
        }
        if (m.cache_mb > 0 && m.cache_mb < MoeStreamConfig::cache_min_mb && !m.force_cache) {
            return fail("moe.cache_mb=" + std::to_string(m.cache_mb) + " is in the pathological band (< " +
                        std::to_string(MoeStreamConfig::cache_min_mb) +
                        " MiB): a cache smaller than one token's routed working set thrashes and is "
                        "slower than no cache. Use 0 to disable the cache, a value >= " +
                        std::to_string(MoeStreamConfig::cache_min_mb) + ", or set force_cache to override.");
        }
        if (m.prefetch_layers < 0 || m.prefetch_layers > MoeStreamConfig::prefetch_layers_max) {
            return fail("moe.prefetch_layers must be in [0, " + std::to_string(MoeStreamConfig::prefetch_layers_max) +
                        "]");
        }
        if (m.cache_auto && m.cache_mb > 0) {
            return fail("moe.cache_auto and an explicit moe.cache_mb are mutually exclusive: choose "
                        "auto-sizing (cache_mb = 0, cache_auto) or a fixed budget (cache_mb > 0).");
        }
        if (m.cache_floor_mb < 0) {
            return fail("moe.cache_floor_mb must be >= 0");
        }
        if (m.cache_ceil_mb < 0) {
            return fail("moe.cache_ceil_mb must be >= 0 (0 = no explicit ceiling)");
        }
        // "The LRU cache is on" means a fixed budget OR auto-sizing (which sizes a real LRU cache).
        const bool cache_on = m.cache_mb > 0 || m.cache_auto;
        if (m.prefetch_layers > 0 && !cache_on) {
            return fail("moe.prefetch_layers requires the LRU cache (cache_mb > 0 or cache_auto): "
                        "speculative reads land in the per-layer cache buffers, which do not exist "
                        "with the cache off.");
        }
        if (m.cache_dynamic && !cache_on) {
            return fail("moe.cache_dynamic requires the LRU cache (cache_mb > 0 or cache_auto): it "
                        "sizes a cache budget at runtime, and there is no budget to size with the "
                        "cache off.");
        }
    }

    return r;
}

} // namespace bmoe
