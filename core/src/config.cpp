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
    }

    return r;
}

} // namespace bmoe
