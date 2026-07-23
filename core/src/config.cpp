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

    // Sampling ranges are enforced only when sampling is actually on (temp > 0). With temp <= 0
    // the engine takes the argmax path and the other knobs are inert, so a caller that leaves them
    // at any value still describes a valid greedy run — today's default stays valid untouched.
    if (cfg.sampling.temp > 0.0f) {
        if (cfg.sampling.top_k < 0) {
            return fail("sampling.top_k must be >= 0 (0 disables the top-k stage)");
        }
        if (cfg.sampling.top_p <= 0.0f || cfg.sampling.top_p > 1.0f) {
            return fail("sampling.top_p must be in (0, 1]");
        }
    }

    // overlap is meaningless without streaming (it gates the streamer's own reads). The
    // hook-availability check is deferred to run(): validate() stays pure (no native).
    if (cfg.moe.overlap && !cfg.moe.enabled) {
        return fail("moe.overlap requires moe.enabled");
    }

    // The probe rides on the routing nodes the streamer already isolates. Off the streaming path
    // there is nothing for it to attach to — and nothing to learn either: routing does not depend
    // on how the weights got into memory, so a dense run would only reproduce the same numbers
    // more slowly.
    if (cfg.moe.predict_log && !cfg.moe.enabled) {
        return fail("moe.predict_log requires moe.enabled");
    }
    if (cfg.moe.predict_prefetch && !cfg.moe.enabled) {
        return fail("moe.predict_prefetch requires moe.enabled");
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
        if (m.predict_prefetch && !cache_on) {
            return fail("moe.predict_prefetch requires the LRU cache (cache_mb > 0 or cache_auto): "
                        "speculative reads land in the per-layer cache buffers, which do not exist "
                        "with the cache off.");
        }
        if (m.predict_spec_max < 0 || m.predict_spec_max > 8) {
            return fail("moe.predict_spec_max must be in [0, 8] (0 = retention only, no speculation)");
        }
        if (m.predict_prefetch && m.prefetch_layers > 0) {
            return fail("moe.predict_prefetch and moe.prefetch_layers are mutually exclusive: they "
                        "are two predictors for the same speculative read lanes, and running both "
                        "doubles the speculated bytes for the same future.");
        }
        if (m.drop_cold_frac > 0.0f && !cache_on) {
            return fail("moe.drop_cold_frac requires the LRU cache (cache_mb > 0 or cache_auto): with the "
                        "cache off every expert is a miss, so the policy stops being cache-aware and "
                        "degenerates into an unconditional weight cut — which is what n_expert_used already "
                        "does, without pretending to consult residency.");
        }
        // Written as a negated inclusive range so NaN (every comparison false) is rejected too,
        // instead of slipping past both this check and the cache_on one above.
        if (!(m.drop_cold_frac >= 0.0f && m.drop_cold_frac <= 1.0f)) {
            return fail("moe.drop_cold_frac must be in [0, 1] (0 = off). Above 1.0 the threshold can "
                        "exceed the largest weight in a routing, which would discard every expert of a "
                        "layer; 1.0 is the uniform share 1/n_expert_used and the useful maximum.");
        }
    }

    return r;
}

} // namespace bmoe
