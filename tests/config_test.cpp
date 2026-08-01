// Unit tests for validate() (core/src/config.cpp) — the pure config-consistency gate.
//
// config.h has long advertised validate() as "unit-tested without the native backend"; this is
// that test. It needs no model and no llama.cpp, so it runs unconditionally in ctest. It covers the
// pre-existing rules and the opt-in sampling ranges added in #51.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "bmoe/config.h"

#include <cstdio>
#include <limits>
#include <string>

using namespace bmoe;

static int failures = 0;

// A config that passes validate(): the minimum is a model path. Streaming off, greedy sampling.
static RunConfig ok_base() {
    RunConfig c;
    c.model_path = "model.gguf";
    return c;
}

// A config with MoE streaming on, otherwise valid — the base for the streaming-rule cases.
static RunConfig ok_moe() {
    RunConfig c = ok_base();
    c.moe.enabled = true;
    return c;
}

static void expect_ok(const char * name, const RunConfig & c) {
    ValidationResult r = validate(c);
    if (r.ok) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n  expected ok, got error: %s\n", name, r.error.c_str());
        ++failures;
    }
}

static void expect_fail(const char * name, const RunConfig & c) {
    ValidationResult r = validate(c);
    if (!r.ok) {
        std::printf("[PASS] %s (rejected: %s)\n", name, r.error.c_str());
    } else {
        std::printf("[FAIL] %s\n  expected rejection, got ok\n", name);
        ++failures;
    }
}

int main() {
    // Baseline and the pre-existing scalar rules.
    expect_ok("valid minimal config", ok_base());
    {
        RunConfig c = ok_base();
        c.model_path.clear();
        expect_fail("empty model_path", c);
    }
    {
        RunConfig c = ok_base();
        c.n_predict = 0;
        expect_fail("n_predict must be positive", c);
    }
    {
        RunConfig c = ok_base();
        c.n_threads = 0;
        expect_fail("n_threads must be positive", c);
    }
    {
        RunConfig c = ok_base();
        c.n_ctx = -1;
        expect_fail("n_ctx must be positive", c);
    }
    {
        RunConfig c = ok_base();
        c.n_expert_used = -1;
        expect_fail("n_expert_used must be >= 0", c);
    }

    // Streaming rules.
    {
        RunConfig c = ok_base();
        c.moe.overlap = true; // enabled stays false
        expect_fail("overlap requires streaming", c);
    }
    expect_ok("streaming, cache off", ok_moe());
    {
        RunConfig c = ok_moe();
        c.moe.io_threads = MoeStreamConfig::io_threads_max + 1;
        expect_fail("io_threads out of range", c);
    }
    {
        RunConfig c = ok_moe();
        c.moe.cache_mb = MoeStreamConfig::cache_min_mb - 1; // pathological band
        expect_fail("cache_mb in pathological band", c);
        c.moe.force_cache = true;
        expect_ok("cache_mb in band allowed with force_cache", c);
    }
    {
        RunConfig c = ok_moe();
        c.moe.cache_auto = true;
        c.moe.cache_mb = 2048; // both set
        expect_fail("cache_auto and explicit cache_mb are mutually exclusive", c);
    }
    {
        RunConfig c = ok_moe();
        c.moe.prefetch_layers = 2; // cache off
        expect_fail("prefetch requires the cache", c);
        c.moe.cache_mb = MoeStreamConfig::cache_min_mb;
        expect_ok("prefetch allowed with the cache on", c);
    }

    // Route-ahead: needs streaming, bounded, excludes the probe and both prefetchers. It does NOT
    // need the cache — committing the routing is orthogonal to how the committed experts get read.
    {
        RunConfig c = ok_base();
        c.moe.route_ahead = 1; // enabled stays false
        expect_fail("route_ahead requires streaming", c);
    }
    {
        RunConfig c = ok_moe();
        c.moe.route_ahead = 1;
        expect_ok("route_ahead with cache off", c);
        c.moe.route_ahead = MoeStreamConfig::route_ahead_max + 1;
        expect_fail("route_ahead out of range", c);
        c.moe.route_ahead = -1;
        expect_fail("route_ahead negative", c);
    }
    {
        RunConfig c = ok_moe();
        c.moe.route_ahead = 1;
        c.moe.predict_log = true;
        expect_fail("route_ahead excludes predict_log", c);
    }
    {
        RunConfig c = ok_moe();
        c.moe.route_ahead = 1;
        c.moe.cache_mb = MoeStreamConfig::cache_min_mb;
        c.moe.predict_prefetch = true;
        expect_fail("route_ahead excludes predict_prefetch", c);
        c.moe.predict_prefetch = false;
        c.moe.prefetch_layers = 2;
        expect_fail("route_ahead excludes prefetch_layers", c);
        c.moe.prefetch_layers = 0;
        expect_ok("route_ahead with the cache and no predictor", c);
    }
    // A verify decode is several positions wide, and route-ahead declines to commit on every one of
    // them while still paying for the prediction and the early reads — measured, see config.cpp.
    {
        RunConfig c = ok_moe();
        c.moe.route_ahead = 1;
        c.spec.source = DraftSource::mtp;
        expect_fail("route_ahead excludes the MTP draft source", c);
        c.spec.source = DraftSource::ngram;
        expect_fail("route_ahead excludes the n-gram draft source", c);
        c.spec.source = DraftSource::none;
        expect_ok("route_ahead with speculation off", c);
    }

    // Sampling ranges — enforced only when temp > 0.
    {
        RunConfig c = ok_base();
        c.sampling.temp = 0.8f;
        c.sampling.top_k = 40;
        c.sampling.top_p = 0.95f;
        expect_ok("valid sampling config", c);
    }
    {
        RunConfig c = ok_base();
        c.sampling.temp = 0.8f;
        c.sampling.top_p = 0.0f;
        expect_fail("top_p must be > 0 when sampling", c);
        c.sampling.top_p = 1.5f;
        expect_fail("top_p must be <= 1 when sampling", c);
    }
    {
        RunConfig c = ok_base();
        c.sampling.temp = 0.8f;
        c.sampling.top_k = -1;
        expect_fail("top_k must be >= 0 when sampling", c);
    }
    {
        // With greedy (temp <= 0) the other knobs are inert, so out-of-range values are still a
        // valid greedy run — the default path must never be rejected on account of sampling fields.
        RunConfig c = ok_base();
        c.sampling.temp = 0.0f;
        c.sampling.top_p = 5.0f;
        c.sampling.top_k = -3;
        expect_ok("out-of-range sampling knobs are inert under greedy", c);
    }

    // Cache-aware expert dropping. The upper bound is not cosmetic: above the uniform share the
    // threshold can exceed every weight in a routing, and a config that can empty a layer must not
    // be accepted just because the implementation happens to guard against it too.
    {
        RunConfig c = ok_base();
        c.moe.enabled = true;
        c.moe.drop_cold_frac = 0.5f;
        expect_fail("dropping without a cache is rejected (nothing to be aware of)", c);

        c.moe.cache_mb = MoeStreamConfig::cache_min_mb;
        c.moe.drop_cold_frac = 0.0f;
        expect_ok("dropping off is the default and valid", c);
        c.moe.drop_cold_frac = 0.5f;
        expect_ok("a threshold below the uniform share is valid", c);
        c.moe.drop_cold_frac = 1.0f;
        expect_ok("the uniform share itself is valid", c);
        c.moe.drop_cold_frac = 1.01f;
        expect_fail("a threshold above the uniform share is rejected", c);
        c.moe.drop_cold_frac = -0.1f;
        expect_fail("a negative threshold is rejected", c);
        // NaN compares false against everything, so a plain min/max pair would wave it through —
        // and it would also skip the cache_on requirement above for the same reason.
        c.moe.drop_cold_frac = std::numeric_limits<float>::quiet_NaN();
        expect_fail("a NaN threshold is rejected", c);
    }

    // n_ubatch: 0 follows the context; a value above it would reserve compute buffers for a batch
    // that cannot occur, which inverts the memory saving the knob exists for.
    {
        RunConfig c = ok_base();
        expect_ok("n_ubatch 0 (follow the context) is the default and valid", c);
        c.n_ubatch = 512;
        expect_ok("an n_ubatch below n_ctx is valid", c);
        c.n_ubatch = c.n_ctx;
        expect_ok("an n_ubatch equal to n_ctx is valid", c);
        c.n_ubatch = c.n_ctx + 1;
        expect_fail("an n_ubatch above n_ctx is rejected", c);
        c.n_ubatch = -1;
        expect_fail("a negative n_ubatch is rejected", c);
    }

    // Speculation: lossless only under greedy, and the draft width is bounded on both sides.
    // These rules are shared by every draft source, so they are checked against the MTP one.
    {
        RunConfig c = ok_base();
        expect_ok("speculation off is the default and valid", c);
        c.spec.source = DraftSource::mtp;
        expect_ok("speculation on with the default draft width is valid", c);
        c.spec.draft_max = SpecConfig::draft_max_limit;
        expect_ok("the largest allowed draft width is valid", c);
        c.spec.draft_max = SpecConfig::draft_max_limit + 1;
        expect_fail("a draft width above the limit is rejected", c);
        c.spec.draft_max = 0;
        expect_fail("drafting zero tokens is rejected", c);
        c.spec.draft_max = 3;
        // The confidence floor is a probability, so both ends are bounded.
        c.spec.draft_p_min = 0.0f;
        expect_ok("no confidence floor is the default and valid", c);
        c.spec.draft_p_min = 0.6f;
        expect_ok("a confidence floor inside (0,1) is valid", c);
        c.spec.draft_p_min = 1.0f;
        expect_ok("a confidence floor of 1 is valid (draft only what is certain)", c);
        c.spec.draft_p_min = 1.5f;
        expect_fail("a confidence floor above 1 is rejected", c);
        c.spec.draft_p_min = -0.1f;
        expect_fail("a negative confidence floor is rejected", c);
        c.spec.draft_p_min = 0.0f;
        c.sampling.temp = 0.8f;
        expect_fail("speculation with a sampling chain is rejected", c);
        // The same temperature is fine once speculation is off: the rejection is about the pair.
        c.spec.source = DraftSource::none;
        expect_ok("sampling without speculation stays valid", c);
    }

    // The n-gram source: the same shared rules, plus its own gate, and no cross-talk with the
    // MTP-only knob — a flag the chosen source cannot act on is rejected, not silently ignored.
    {
        RunConfig c = ok_base();
        c.spec.source = DraftSource::ngram;
        expect_ok("the n-gram source with its defaults is valid", c);
        c.sampling.temp = 0.8f;
        expect_fail("the n-gram source with a sampling chain is rejected", c);
        c.sampling.temp = 0.0f;
        c.spec.draft_max = SpecConfig::draft_max_limit + 1;
        expect_fail("the draft-width limit applies to the n-gram source too", c);
        c.spec.draft_max = 3;
        c.spec.draft_p_min = 0.6f;
        expect_fail("the MTP confidence floor is rejected with the n-gram source", c);
        c.spec.draft_p_min = 0.0f;
        c.spec.ngram_min_match = 1;
        expect_ok("a minimum match of 1 is valid (draft on any repeated token)", c);
        c.spec.ngram_min_match = 0;
        expect_fail("a minimum match of 0 is rejected", c);
        c.spec.ngram_min_match = c.spec.ngram_max_match + 1;
        expect_fail("a minimum match above the longest suffix considered is rejected", c);
        c.spec.ngram_min_match = 3;
        c.spec.ngram_max_match = SpecConfig::ngram_match_limit + 1;
        expect_fail("a maximum match above the limit is rejected", c);
        c.spec.ngram_max_match = 0;
        expect_fail("a maximum match of 0 is rejected", c);
        c.spec.ngram_max_match = 12;
        expect_ok("the n-gram bounds back in range are valid", c);
        // The n-gram knobs are inert for the MTP source, so asking for both is a caller error.
        c.spec.source = DraftSource::mtp;
        c.spec.draft_p_min = 0.6f;
        expect_ok("the confidence floor is valid for the source that has one", c);
    }

    // The verify batch must fit in one graph, or the amortisation it exists for is split away.
    {
        RunConfig c = ok_base();
        c.spec.source = DraftSource::mtp;
        c.spec.draft_max = 3;
        expect_ok("the default ubatch (0 = as wide as the context) is valid with speculation", c);
        c.n_ubatch = 4; // exactly 1 + draft_max
        expect_ok("a ubatch exactly as wide as the verify batch is valid", c);
        c.n_ubatch = 3;
        expect_fail("a ubatch narrower than the verify batch is rejected", c);
        // The rule is about the verify batch, so it binds the n-gram source identically.
        c.spec.source = DraftSource::ngram;
        expect_fail("the narrow ubatch is rejected with the n-gram source too", c);
        c.spec.source = DraftSource::none;
        expect_ok("the same narrow ubatch is fine without speculation", c);
    }

    if (failures == 0) {
        std::printf("all config checks passed\n");
        return 0;
    }
    std::printf("%d config check(s) failed\n", failures);
    return 1;
}
