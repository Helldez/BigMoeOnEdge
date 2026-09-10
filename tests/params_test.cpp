// Unit tests for the parameter table (core/include/bmoe/params.h).
//
// Three properties, each of which a front-end silently depends on:
//   1. The table is well-formed: unique keys and flags, defaults inside their own bounds, every
//      default readable back through its own setter.
//   2. Every flag the CLI parsed by hand before the table existed sets the same field to the same
//      value. The expectations below are the old parser's semantics, written out one flag at a time.
//   3. to_args() reads back into the config it came from, so "copy as command" and a bench cell
//      reproduce a form's settings exactly.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "bmoe/config.h"
#include "bmoe/params.h"

#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace bmoe;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    if (ok) {
        std::printf("[PASS] %s\n", what.c_str());
    } else {
        std::printf("[FAIL] %s\n", what.c_str());
        ++failures;
    }
}

// Parse argv the way a front-end does: known flags through the table, nothing else expected here.
static bool parse(RunConfig & cfg, const std::vector<std::string> & argv, std::string & err) {
    for (size_t i = 0; i < argv.size(); ++i) {
        const char * next = i + 1 < argv.size() ? argv[i + 1].c_str() : nullptr;
        FlagResult r = apply_flag(cfg, argv[i].c_str(), next);
        if (!r.matched) {
            err = "unknown flag " + argv[i];
            return false;
        }
        if (!r.error.empty()) {
            err = r.error;
            return false;
        }
        if (r.consumed_value) ++i;
    }
    return true;
}

static std::string join(const std::vector<std::string> & v) {
    std::string s;
    for (const std::string & a : v)
        s += (s.empty() ? "" : " ") + a;
    return s;
}

// One old-parser expectation: these arguments, applied to a default config, must satisfy `holds`.
static void expect_flag(const std::vector<std::string> & argv, const std::function<bool(const RunConfig &)> & holds) {
    RunConfig c;
    std::string err;
    const bool ok = parse(c, argv, err);
    check(ok && holds(c), "flag " + join(argv) + (ok ? "" : " (" + err + ")"));
}

static void expect_reject(const std::vector<std::string> & argv) {
    RunConfig c;
    std::string err;
    const bool ok = parse(c, argv, err);
    check(!ok, "rejects " + join(argv) + (ok ? "" : " (" + err + ")"));
}

// Every described parameter, compared by its rendered value.
static bool same_params(const RunConfig & a, const RunConfig & b, std::string & diff) {
    for (const ParamDesc & d : params()) {
        if (d.get(a) != d.get(b)) {
            diff = d.key + ": " + d.get(a) + " vs " + d.get(b);
            return false;
        }
    }
    return true;
}

static void expect_roundtrip(const char * name, const RunConfig & cfg) {
    const std::vector<std::string> args = to_args(cfg);
    RunConfig back;
    std::string err, diff;
    const bool parsed = parse(back, args, err);
    const bool same = parsed && same_params(cfg, back, diff);
    check(same, std::string("to_args round trip: ") + name + " [" + join(args) + "]" +
                    (parsed ? (same ? "" : " differs at " + diff) : " parse failed: " + err));
}

int main() {
    // ── 1. the table is well-formed ──────────────────────────────────────────────────
    {
        std::set<std::string> keys, flags;
        bool unique_keys = true, unique_flags = true;
        for (const ParamDesc & d : params()) {
            unique_keys &= keys.insert(d.key).second;
            if (!d.flag.empty()) unique_flags &= flags.insert(d.flag).second;
            if (!d.short_flag.empty()) unique_flags &= flags.insert(d.short_flag).second;
            for (const ParamSwitch & s : d.switches)
                unique_flags &= flags.insert(s.flag).second;
        }
        check(unique_keys, "keys are unique");
        check(unique_flags, "flags and switches are unique across the table");
    }
    const RunConfig defaults;
    for (const ParamDesc & d : params()) {
        check(d.get && d.set, d.key + ": has accessors");
        check(!d.flag.empty() || !d.switches.empty(), d.key + ": reachable from the command line");

        // The default reads back through the setter to itself.
        RunConfig c;
        std::string err;
        const std::string v = d.get(defaults);
        check(d.set(c, v, err) && d.get(c) == v, d.key + ": default '" + v + "' round-trips");

        if (d.bounded && (d.type == ParamType::Int || d.type == ParamType::Float) && v != "auto") {
            const double x = std::stod(v);
            check(x >= d.min && x <= d.max, d.key + ": default inside [min, max]");
        }
        if (d.type == ParamType::Choice) {
            bool listed = false;
            for (const ParamChoice & ch : d.choices)
                listed |= ch.value == v;
            check(listed, d.key + ": default is one of its choices");
        }
        // Every switch sets a value the setter accepts.
        for (const ParamSwitch & s : d.switches) {
            RunConfig cs;
            check(d.set(cs, s.value, err), d.key + ": switch " + s.flag + " sets a valid value");
        }
    }
    // Keys follow the planner's naming: the long flag without dashes.
    for (const ParamDesc & d : params())
        if (!d.flag.empty()) check(d.flag == "--" + d.key, d.key + ": flag is --key");

    // ── 2. the old parser's semantics, flag by flag ───────────────────────────────────
    expect_flag({"-m", "x.gguf"}, [](const RunConfig & c) { return c.model_path == "x.gguf"; });
    expect_flag({"--model", "y.gguf"}, [](const RunConfig & c) { return c.model_path == "y.gguf"; });
    expect_flag({"-n", "64"}, [](const RunConfig & c) { return c.n_predict == 64; });
    expect_flag({"--n-predict", "65"}, [](const RunConfig & c) { return c.n_predict == 65; });
    expect_flag({"-t", "6"}, [](const RunConfig & c) { return c.n_threads == 6; });
    expect_flag({"--threads", "7"}, [](const RunConfig & c) { return c.n_threads == 7; });
    expect_flag({"-c", "4096"}, [](const RunConfig & c) { return c.n_ctx == 4096; });
    expect_flag({"--ctx-size", "1024"}, [](const RunConfig & c) { return c.n_ctx == 1024; });
    expect_flag({"--ubatch", "256"}, [](const RunConfig & c) { return c.n_ubatch == 256; });
    expect_flag({"--n-expert-used", "4"}, [](const RunConfig & c) { return c.n_expert_used == 4; });
    expect_flag({"--temp", "0.7"}, [](const RunConfig & c) { return c.sampling.temp == 0.7f; });
    expect_flag({"--top-k", "20"}, [](const RunConfig & c) { return c.sampling.top_k == 20; });
    expect_flag({"--top-p", "0.9"}, [](const RunConfig & c) { return c.sampling.top_p == 0.9f; });
    expect_flag({"--seed", "42"}, [](const RunConfig & c) { return c.sampling.seed == 42u; });
    expect_flag({"--seed", "4294967295"}, [](const RunConfig & c) { return c.sampling.seed == 0xFFFFFFFFu; });
    expect_flag({"--mtp"}, [](const RunConfig & c) { return c.spec.source == DraftSource::mtp; });
    expect_flag({"--ngram"}, [](const RunConfig & c) { return c.spec.source == DraftSource::ngram; });
    expect_flag({"--draft", "5"}, [](const RunConfig & c) { return c.spec.draft_max == 5; });
    expect_flag({"--mtp-p-min", "0.6"}, [](const RunConfig & c) { return c.spec.draft_p_min == 0.6f; });
    expect_flag({"--ngram-min-match", "4"}, [](const RunConfig & c) { return c.spec.ngram_min_match == 4; });
    expect_flag({"--ngram-max-match", "16"}, [](const RunConfig & c) { return c.spec.ngram_max_match == 16; });
    expect_flag({"--chatml"}, [](const RunConfig & c) { return c.chatml; });
    expect_flag({"--no-think"}, [](const RunConfig & c) { return !c.think; });
    expect_flag({"--moe-stream"}, [](const RunConfig & c) { return c.moe.enabled; });
    expect_flag({"--cache-mb", "auto"}, [](const RunConfig & c) { return c.moe.cache_auto && c.moe.cache_mb == 0; });
    expect_flag({"--cache-mb", "2000"},
                [](const RunConfig & c) { return !c.moe.cache_auto && c.moe.cache_mb == 2000; });
    expect_flag({"--cache-mb", "0"}, [](const RunConfig & c) { return !c.moe.cache_auto && c.moe.cache_mb == 0; });
    expect_flag({"--cache-floor-mb", "1024"}, [](const RunConfig & c) { return c.moe.cache_floor_mb == 1024; });
    expect_flag({"--cache-ceil-mb", "3000"}, [](const RunConfig & c) { return c.moe.cache_ceil_mb == 3000; });
    expect_flag({"--io-threads", "2"}, [](const RunConfig & c) { return c.moe.io_threads == 2; });
    expect_flag({"--no-odirect"}, [](const RunConfig & c) { return !c.moe.o_direct; });
    expect_flag({"--release-mmap"}, [](const RunConfig & c) { return c.moe.release_mmap; });
    expect_flag({"--row-stream"}, [](const RunConfig & c) { return c.moe.row_stream; });
    expect_flag({"--row-stream-mb", "32"}, [](const RunConfig & c) { return c.moe.row_stream_mb == 32; });
    expect_flag({"--dense-weights", "mmap"},
                [](const RunConfig & c) { return c.moe.dense_weights == DenseWeightsMode::Mmap; });
    expect_flag({"--dense-weights", "warm"},
                [](const RunConfig & c) { return c.moe.dense_weights == DenseWeightsMode::Warmed; });
    expect_flag({"--dense-weights", "anon"},
                [](const RunConfig & c) { return c.moe.dense_weights == DenseWeightsMode::Anonymous; });
    expect_flag({"--dense-weights", "ahwb"},
                [](const RunConfig & c) { return c.moe.dense_weights == DenseWeightsMode::Pinned; });
    expect_flag({"--no-warm-dense"}, [](const RunConfig & c) { return c.moe.dense_weights == DenseWeightsMode::Mmap; });
    expect_flag({"--dense-weights", "mmap", "--dense-odirect"},
                [](const RunConfig & c) { return c.moe.dense_weights == DenseWeightsMode::Anonymous; });
    expect_flag({"--load-all"}, [](const RunConfig & c) { return c.moe.load_all; });
    expect_flag({"--force-cache"}, [](const RunConfig & c) { return c.moe.force_cache; });
    expect_flag({"--overlap"}, [](const RunConfig & c) { return c.moe.overlap; });
    expect_flag({"--io-two-wave"}, [](const RunConfig & c) { return c.moe.io_two_wave; });
    expect_flag({"--prefetch", "2"}, [](const RunConfig & c) { return c.moe.prefetch_layers == 2; });
    expect_flag({"--prefetch-sync"}, [](const RunConfig & c) { return c.moe.prefetch_sync; });
    expect_flag({"--drop-cold-experts", "0.75"}, [](const RunConfig & c) { return c.moe.drop_cold_frac == 0.75f; });
    expect_flag({"--expert-substitute", "0.15"}, [](const RunConfig & c) { return c.moe.substitute_lambda == 0.15f; });
    expect_flag({"--drop-no-renorm"}, [](const RunConfig & c) { return !c.moe.drop_renorm; });
    expect_flag({"--drop-in-prefill"}, [](const RunConfig & c) { return c.moe.drop_prefill; });
    expect_flag({"--route-ahead", "2"}, [](const RunConfig & c) { return c.moe.route_ahead == 2; });
    expect_flag({"--predict-log"}, [](const RunConfig & c) { return c.moe.predict_log; });
    expect_flag({"--predict-prefetch"}, [](const RunConfig & c) { return c.moe.predict_prefetch; });
    expect_flag({"--predict-spec-max", "0"}, [](const RunConfig & c) { return c.moe.predict_spec_max == 0; });

    // Flags the table must NOT claim: they belong to the front-end, not to a run's configuration.
    for (const char * f : {"-p", "--prompt", "--progress", "--session", "--csv", "--route-trace", "--compute-trace",
                           "--compute-trace-layers", "--io-trace", "--ppl", "--list-archs", "--help", "--version"}) {
        RunConfig c;
        check(!apply_flag(c, f, "x").matched, std::string("front-end flag ") + f + " is not in the table");
    }

    // Malformed values are errors, not atoi()'s silent 0.
    expect_reject({"--threads", "4x"});
    expect_reject({"--threads", "abc"});
    expect_reject({"--threads"});
    expect_reject({"--cache-mb", "big"});
    expect_reject({"--dense-weights", "foo"});
    expect_reject({"--temp", "hot"});
    expect_reject({"--seed", "-1"});
    expect_reject({"--seed", "4294967296"});

    // ── 3. to_args() reads back into the config it came from ─────────────────────────
    expect_roundtrip("defaults", RunConfig{});
    {
        RunConfig c;
        c.model_path = "models/q.gguf";
        c.n_ctx = 4096;
        c.n_threads = 8;
        c.think = false;
        c.chatml = true;
        c.moe.enabled = true;
        c.moe.cache_auto = true;
        c.moe.cache_ceil_mb = 3000;
        c.moe.overlap = true;
        c.moe.o_direct = false;
        c.moe.dense_weights = DenseWeightsMode::Pinned;
        c.moe.release_mmap = true;
        expect_roundtrip("streaming app-like", c);
    }
    {
        RunConfig c;
        c.sampling.temp = 0.7f;
        c.sampling.top_p = 0.9f;
        c.sampling.seed = 7;
        c.moe.enabled = true;
        c.moe.cache_mb = 2500;
        c.moe.drop_cold_frac = 0.75f;
        c.moe.drop_renorm = false;
        c.moe.substitute_lambda = 0.15f;
        c.n_expert_used = 6;
        expect_roundtrip("sampling + lossy", c);
    }
    {
        RunConfig c;
        c.spec.source = DraftSource::ngram;
        c.spec.draft_max = 4;
        c.spec.ngram_min_match = 2;
        c.spec.ngram_max_match = 20;
        c.n_ubatch = 128;
        c.moe.enabled = true;
        c.moe.predict_prefetch = true;
        c.moe.predict_spec_max = 0;
        c.moe.row_stream = true;
        c.moe.row_stream_mb = 16;
        expect_roundtrip("speculation + prefetch", c);
    }
    {
        // Deprecated switches parse but are never emitted.
        RunConfig c;
        c.moe.dense_weights = DenseWeightsMode::Mmap;
        const std::vector<std::string> args = to_args(c);
        check(join(args) == "--dense-weights mmap", "to_args prefers --dense-weights over the deprecated alias");
    }

    // ── schema ───────────────────────────────────────────────────────────────────────
    {
        const std::string js = params_json(RunConfig{});
        bool all = js.size() > 2 && js.front() == '[' && js.back() == ']';
        for (const ParamDesc & d : params())
            all &= js.find("\"key\":\"" + d.key + "\"") != std::string::npos;
        check(all, "params_json lists every key");
        RunConfig c;
        c.moe.cache_auto = true;
        check(params_json(c).find("\"default\":\"auto\"") != std::string::npos, "params_json takes caller defaults");
        check(config_json(RunConfig{}).find("\"moe-stream\":false") != std::string::npos, "config_json types bools");
    }

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall parameter-table checks passed (%zu parameters)\n", params().size());
    return 0;
}
