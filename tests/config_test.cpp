// Unit tests for validate() (core/src/config.cpp) — the pure config-consistency gate.
//
// config.h has long advertised validate() as "unit-tested without the native backend"; this is
// that test. It needs no model and no llama.cpp, so it runs unconditionally in ctest. It covers the
// pre-existing rules and the opt-in sampling ranges added in #51.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "bmoe/config.h"
#include "bmoe/recipe.h"

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

    // GPU offload. Availability is a runtime question, so validate() only settles coherence.
    {
        RunConfig c = ok_base();
        expect_ok("gpu off is the default and valid", c);

        c.gpu.enabled = true;
        expect_ok("gpu on with the default layer count is valid", c);
        c.gpu.n_layers = 0;
        expect_ok("gpu with zero layers is valid (offload nothing, still a legal request)", c);
        c.gpu.n_layers = 24;
        expect_ok("gpu with an explicit layer count is valid", c);
        c.gpu.n_layers = -2;
        expect_fail("a layer count below -1 is rejected", c);

        c = ok_base();
        c.gpu.require = true;
        expect_fail("gpu.require without gpu.enabled is rejected", c);
        c = ok_base();
        c.gpu.n_layers = 24;
        expect_fail("gpu.n_layers without gpu.enabled is rejected", c);

        // The offload is orthogonal to streaming: it moves the half of the model the streamer
        // never touches, so every combination of the two is legal.
        c = ok_moe();
        c.moe.cache_mb = MoeStreamConfig::cache_min_mb;
        c.gpu.enabled = true;
        expect_ok("gpu offload combines with expert streaming", c);
    }

    // The expert-tensor pattern is derived from the recipe table, so every registered
    // architecture must produce one — a row that pinned nothing would silently offload its
    // experts to a device the streamer cannot rebind.
    {
        for (int i = 0; i < n_moe_recipes(); ++i) {
            const MoeRecipe * r = moe_recipe_at(i);
            const std::string pat = expert_tensor_pattern(*r);
            const bool ok = !pat.empty() && pat.front() == '\\' && pat.back() == '.';
            std::printf("%s expert pattern for %s: %s\n", ok ? "[PASS]" : "[FAIL]", r->arch, pat.c_str());
            if (!ok) ++failures;

            // The CPU pin must additionally carry the router: the streamer reads the routing node
            // from the host, so leaving `ffn_gate_inp` on a device backend is a wild pointer, not a
            // slow path. Its absence is exactly the bug that segfaulted the first GPU run.
            const std::string pin = cpu_pinned_tensor_pattern(*r);
            const bool pin_ok = pin.find("ffn_gate_inp") != std::string::npos && pin.back() == '.' &&
                                pin.find(r->exps_suffix[0]) != std::string::npos;
            std::printf("%s cpu pin for %s: %s\n", pin_ok ? "[PASS]" : "[FAIL]", r->arch, pin.c_str());
            if (!pin_ok) ++failures;
        }
        // A recipe naming nothing must match nothing rather than everything — the failure mode
        // that would quietly pin the whole model to the CPU.
        const MoeRecipe empty{"none", {nullptr, nullptr, nullptr}};
        if (!cpu_pinned_tensor_pattern(empty).empty()) {
            std::printf("[FAIL] an empty recipe must yield an empty cpu pin\n");
            ++failures;
        }
        if (!expert_tensor_pattern(empty).empty()) {
            std::printf("[FAIL] an empty recipe must yield an empty pattern\n");
            ++failures;
        } else {
            std::printf("[PASS] an empty recipe yields an empty pattern\n");
        }
    }

    if (failures == 0) {
        std::printf("all config checks passed\n");
        return 0;
    }
    std::printf("%d config check(s) failed\n", failures);
    return 1;
}
