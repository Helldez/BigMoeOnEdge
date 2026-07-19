// Unit tests for validate() (core/src/config.cpp) — the pure config-consistency gate.
//
// config.h has long advertised validate() as "unit-tested without the native backend"; this is
// that test. It needs no model and no llama.cpp, so it runs unconditionally in ctest. It covers the
// pre-existing rules and the opt-in sampling ranges added in #51.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "bmoe/config.h"

#include <cstdio>
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
    // reasoning_budget: -1 is the "unlimited" sentinel and 0 ("close the block at once") is a
    // real request, so only values below the sentinel are nonsense.
    {
        RunConfig c = ok_base();
        c.reasoning_budget = -2;
        expect_fail("reasoning_budget below the unlimited sentinel", c);
    }
    {
        RunConfig c = ok_base();
        c.reasoning_budget = 0;
        expect_ok("reasoning_budget 0 suppresses reasoning", c);
    }
    {
        RunConfig c = ok_base();
        c.reasoning_budget = 512;
        expect_ok("reasoning_budget caps reasoning", c);
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

    if (failures == 0) {
        std::printf("all config checks passed\n");
        return 0;
    }
    std::printf("%d config check(s) failed\n", failures);
    return 1;
}
