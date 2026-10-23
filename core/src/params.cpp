#include "bmoe/params.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

namespace bmoe {

namespace {

// ── value codecs ────────────────────────────────────────────────────────────────────
// Strict on purpose: the whole string must be the number. A form that sends "4x" must get an error
// back, not a silently truncated 4 — or, worse, the 0 that atoi() makes of "abc".

bool parse_int(const std::string & s, long long lo, long long hi, long long & out, std::string & err) {
    if (s.empty()) {
        err = "expects an integer, got an empty value";
        return false;
    }
    errno = 0;
    char * end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') {
        err = "expects an integer, got '" + s + "'";
        return false;
    }
    if (v < lo || v > hi) {
        err = "value " + s + " does not fit the field";
        return false;
    }
    out = v;
    return true;
}

bool parse_float(const std::string & s, float & out, std::string & err) {
    if (s.empty()) {
        err = "expects a number, got an empty value";
        return false;
    }
    errno = 0;
    char * end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str() || *end != '\0' || std::isnan(v)) {
        err = "expects a number, got '" + s + "'";
        return false;
    }
    out = (float) v;
    return true;
}

bool parse_bool(const std::string & s, bool & out, std::string & err) {
    if (s == "true" || s == "1" || s == "on") {
        out = true;
        return true;
    }
    if (s == "false" || s == "0" || s == "off") {
        out = false;
        return true;
    }
    err = "expects true|false, got '" + s + "'";
    return false;
}

// Shortest decimal that reads back as the same float, so get() -> set() is exact and a default of
// 0.95 renders as "0.95", not "0.949999988".
std::string format_float(float v) {
    char buf[32];
    for (int prec = 1; prec <= 9; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, (double) v);
        if (std::strtof(buf, nullptr) == v) break;
    }
    return buf;
}

// Accessors: one lambda naming the field, from which both directions are built.
template <typename T> using Field = T & (*) (RunConfig &);

template <typename T> T & at(Field<T> f, const RunConfig & c) {
    return f(const_cast<RunConfig &>(c)); // read-only use: get() never writes through it
}

void bind_int(ParamDesc & d, Field<int> f) {
    d.get = [f](const RunConfig & c) { return std::to_string(at(f, c)); };
    d.set = [f](RunConfig & c, const std::string & s, std::string & err) {
        long long v = 0;
        if (!parse_int(s, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), v, err)) return false;
        f(c) = (int) v;
        return true;
    };
}

void bind_float(ParamDesc & d, Field<float> f) {
    d.get = [f](const RunConfig & c) { return format_float(at(f, c)); };
    d.set = [f](RunConfig & c, const std::string & s, std::string & err) {
        float v = 0.0f;
        if (!parse_float(s, v, err)) return false;
        f(c) = v;
        return true;
    };
}

void bind_bool(ParamDesc & d, Field<bool> f) {
    d.get = [f](const RunConfig & c) { return std::string(at(f, c) ? "true" : "false"); };
    d.set = [f](RunConfig & c, const std::string & s, std::string & err) {
        bool v = false;
        if (!parse_bool(s, v, err)) return false;
        f(c) = v;
        return true;
    };
}

void bind_string(ParamDesc & d, Field<std::string> f) {
    d.get = [f](const RunConfig & c) { return at(f, c); };
    d.set = [f](RunConfig & c, const std::string & s, std::string &) {
        f(c) = s;
        return true;
    };
}

// An enum field shown as its choices' values. `values` is in the enum's declaration order.
template <typename E> void bind_choice(ParamDesc & d, Field<E> f, std::vector<std::pair<E, ParamChoice>> values) {
    for (const auto & v : values)
        d.choices.push_back(v.second);
    d.get = [f, values](const RunConfig & c) {
        for (const auto & v : values)
            if (v.first == at(f, c)) return v.second.value;
        return std::string();
    };
    d.set = [f, values](RunConfig & c, const std::string & s, std::string & err) {
        for (const auto & v : values)
            if (v.second.value == s) {
                f(c) = v.first;
                return true;
            }
        std::string all;
        for (const auto & v : values)
            all += (all.empty() ? "" : "|") + v.second.value;
        err = "expects " + all + ", got '" + s + "'";
        return false;
    };
}

ParamDesc row(const char * key, const char * label, ParamGroup group, ParamLevel level, std::string help) {
    ParamDesc d;
    d.key = key;
    d.label = label;
    d.group = group;
    d.level = level;
    d.help = std::move(help);
    d.flag = std::string("--") + key;
    return d;
}

ParamDesc & bounds(ParamDesc & d, double lo, double hi, const char * unit = "") {
    d.bounded = true;
    d.min = lo;
    d.max = hi;
    d.unit = unit;
    return d;
}

// A lower limit only: the upper one is the type's, which a form must not present as a range.
ParamDesc & at_least(ParamDesc & d, double lo, const char * unit = "") {
    d.bounded_below = true;
    d.min = lo;
    d.unit = unit;
    return d;
}

ParamDesc & typical(ParamDesc & d, double lo, double hi) {
    d.has_typical = true;
    d.typical_min = lo;
    d.typical_max = hi;
    return d;
}

ParamDesc & special(ParamDesc & d, std::string value, const char * label) {
    d.specials.push_back({std::move(value), label});
    return d;
}

// What a form shows as the usual range of a row. Guidance, not limits: every default sits inside
// (or is one of the row's special values, which the tests check), and where a value was measured
// the range brackets it - read lanes (2 saturate a phone's flash, 4 a PC's), the batch width (512
// to 2048, what the planner picks), route-ahead (2 layers, all speculated experts useful). Rows
// whose right value depends on the model or the machine (the top of the cache ceiling, layers on
// a device, active experts) have none: the plan is their guide.
namespace usual {
constexpr int n_predict_lo = 128, n_predict_hi = 2048;
constexpr int ctx_lo = 2048, ctx_hi = 32768;
constexpr int threads_lo = 4, threads_hi = 8;
constexpr int threads_batch_lo = 4, threads_batch_hi = 16;
constexpr double temp_lo = 0.6, temp_hi = 1.0;
constexpr int top_k_lo = 20, top_k_hi = 100;
constexpr double top_p_lo = 0.8, top_p_hi = 0.95;
constexpr int io_threads_lo = 2, io_threads_hi = 8;
constexpr int cache_hi_mb = 8192;
constexpr int cache_floor_lo_mb = 1024, cache_floor_hi_mb = 3072;
constexpr int ubatch_lo = 512, ubatch_hi = 2048;
constexpr int row_stream_lo_mb = 32, row_stream_hi_mb = 256;
constexpr int prefill_min_tokens_hi = 128;
constexpr int prefill_loaders_lo = 4, prefill_loaders_hi = 8;
constexpr double routed_full_lo = 0.7, routed_full_hi = 0.9;
constexpr int prefetch_lo = 1, prefetch_hi = 2;
constexpr int predict_spec_lo = 1, predict_spec_hi = 4;
constexpr int draft_lo = 2, draft_hi = 4;
constexpr int ngram_min_lo = 2, ngram_min_hi = 4;
constexpr int ngram_max_lo = 8, ngram_max_hi = 16;
constexpr int route_ahead_lo = 1, route_ahead_hi = 2;
} // namespace usual

std::vector<ParamDesc> build() {
    using G = ParamGroup;
    using L = ParamLevel;
    std::vector<ParamDesc> t;

    // ── model ────────────────────────────────────────────────────────────────────────
    {
        ParamDesc d = row("model", "Model file", G::Model, L::Basic,
                          "The gguf to load. For a model split into shards, the first shard.");
        d.type = ParamType::Path;
        d.short_flag = "-m";
        d.value_hint = "PATH";
        bind_string(d, [](RunConfig & c) -> std::string & { return c.model_path; });
        t.push_back(d);
    }

    // ── generation ───────────────────────────────────────────────────────────────────
    {
        ParamDesc d = row("n-predict", "Max tokens", G::Generation, L::Basic, "Tokens to generate per answer.");
        d.short_flag = "-n";
        d.value_hint = "N";
        d.scope = ParamScope::Request;
        bounds(d, 1, RunConfig::tokens_max, "tokens");
        typical(d, usual::n_predict_lo, usual::n_predict_hi);
        bind_int(d, [](RunConfig & c) -> int & { return c.n_predict; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("think", "Thinking", G::Generation, L::Basic,
                          "Render the chat template with reasoning enabled, so a reasoning model emits its "
                          "thinking channel. Off suppresses reasoning at the source.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--no-think", "false"}};
        d.scope = ParamScope::Request;
        bind_bool(d, [](RunConfig & c) -> bool & { return c.think; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("ctx-size", "Context", G::Generation, L::Basic,
                          "Context size in tokens. The KV cache and the compute buffers are reserved for it, "
                          "so a larger context takes RAM from the expert cache.");
        d.short_flag = "-c";
        d.value_hint = "N";
        bounds(d, 1, RunConfig::tokens_max, "tokens");
        typical(d, usual::ctx_lo, usual::ctx_hi);
        bind_int(d, [](RunConfig & c) -> int & { return c.n_ctx; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("threads", "Compute threads", G::Generation, L::Basic, "Compute threads.");
        d.short_flag = "-t";
        d.value_hint = "N";
        bounds(d, 1, RunConfig::threads_max, "threads");
        typical(d, usual::threads_lo, usual::threads_hi);
        bind_int(d, [](RunConfig & c) -> int & { return c.n_threads; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("threads-batch", "Prefill threads", G::Generation, L::Advanced,
                          "Threads for prefill, which is compute-bound and scales with cores, while a streamed "
                          "decode mostly waits on flash. 0 = the same as the compute threads.");
        d.value_hint = "N";
        bounds(d, 0, RunConfig::threads_max, "threads");
        typical(d, usual::threads_batch_lo, usual::threads_batch_hi);
        special(d, "0", "same as the compute threads");
        bind_int(d, [](RunConfig & c) -> int & { return c.n_threads_batch; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("chatml", "Wrap in chat turns", G::Generation, L::Advanced,
                          "Wrap the prompt in the model family's own chat turn. A chat front-end always "
                          "sets it; off sends the prompt as raw text.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--chatml", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.chatml; });
        t.push_back(d);
    }

    // ── sampling ─────────────────────────────────────────────────────────────────────
    {
        ParamDesc d = row("temp", "Temperature", G::Sampling, L::Basic,
                          "Sampling temperature. 0 or below keeps greedy decoding (deterministic); above 0 "
                          "enables the chain top-k -> top-p -> temperature.");
        d.type = ParamType::Float;
        d.value_hint = "F";
        bounds(d, 0.0, SamplingConfig::temp_max);
        typical(d, usual::temp_lo, usual::temp_hi);
        special(d, "0", "greedy");
        bind_float(d, [](RunConfig & c) -> float & { return c.sampling.temp; });
        t.push_back(d);
    }
    {
        ParamDesc d =
            row("top-k", "Top-k", G::Sampling, L::Advanced, "Top-k cutoff when sampling (0 disables the stage).");
        d.value_hint = "N";
        bounds(d, 0, SamplingConfig::top_k_max, "candidates");
        typical(d, usual::top_k_lo, usual::top_k_hi);
        special(d, "0", "off");
        bind_int(d, [](RunConfig & c) -> int & { return c.sampling.top_k; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("top-p", "Top-p", G::Sampling, L::Advanced, "Nucleus cutoff in (0, 1] when sampling.");
        d.type = ParamType::Float;
        d.value_hint = "F";
        bounds(d, 0.0, SamplingConfig::top_p_max);
        typical(d, usual::top_p_lo, usual::top_p_hi);
        special(d, "1", "off");
        bind_float(d, [](RunConfig & c) -> float & { return c.sampling.top_p; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("seed", "Seed", G::Sampling, L::Advanced,
                          "RNG seed for sampling. " + std::to_string(SamplingConfig::seed_random) +
                              " draws a random seed per run.");
        d.value_hint = "N";
        at_least(d, 0);
        special(d, std::to_string(SamplingConfig::seed_random), "random per run");
        d.get = [](const RunConfig & c) { return std::to_string(c.sampling.seed); };
        d.set = [](RunConfig & c, const std::string & s, std::string & err) {
            long long v = 0;
            if (!parse_int(s, 0, std::numeric_limits<uint32_t>::max(), v, err)) return false;
            c.sampling.seed = (uint32_t) v;
            return true;
        };
        t.push_back(d);
    }

    // ── streaming ────────────────────────────────────────────────────────────────────
    {
        ParamDesc d = row("moe-stream", "Expert streaming", G::Streaming, L::Basic,
                          "Stream only the routed experts per token from flash. Off, the engine is plain "
                          "llama.cpp on mmap: a baseline, not this engine.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--moe-stream", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.enabled; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("io-threads", "Read lanes", G::Streaming, L::Advanced,
                          "Parallel expert-read lanes, including the calling thread. 1 is the serial baseline.");
        d.value_hint = "N";
        bounds(d, 1, MoeStreamConfig::io_threads_max, "lanes");
        typical(d, usual::io_threads_lo, usual::io_threads_hi);
        bind_int(d, [](RunConfig & c) -> int & { return c.moe.io_threads; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("o-direct", "Direct I/O", G::Streaming, L::Advanced,
                          "Bypass the page cache for expert reads (O_DIRECT / FILE_FLAG_NO_BUFFERING).");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--no-odirect", "false"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.o_direct; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("overlap", "Overlap I/O", G::Streaming, L::Advanced,
                          "Overlap async expert reads with the FFN compute instead of blocking on them. "
                          "Output is byte-identical. Needs the fork's expert-ready hook in this build.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--overlap", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.overlap; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("io-two-wave", "Start reads in two waves", G::Streaming, L::Experimental,
                          "Publish a layer's first-projection reads before committing the rest, so the lanes "
                          "start sooner. Needs overlap and the cache; pending the on-device A/B.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--io-two-wave", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.io_two_wave; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("release-mmap", "Unmap the file after load", G::Streaming, L::Advanced,
                          "Unmap the model file after load once nothing reads through it. On Windows a live "
                          "mapping serialises the streamer's concurrent reads. Needs dense weights anon or "
                          "ahwb.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--release-mmap", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.release_mmap; });
        t.push_back(d);
    }

    // ── cache ────────────────────────────────────────────────────────────────────────
    {
        ParamDesc d = row("cache-mb", "Expert cache", G::Cache, L::Basic,
                          "LRU expert cache budget in MiB, or auto to size it from the free RAM once at load. "
                          "0 turns the cache off. A budget below " +
                              std::to_string(MoeStreamConfig::cache_min_mb) +
                              " MiB is smaller than one token's working set and thrashes.");
        d.value_hint = "N|auto";
        d.accepts_auto = true;
        at_least(d, 0, "MiB");
        typical(d, MoeStreamConfig::cache_min_mb, usual::cache_hi_mb);
        special(d, "0", "off");
        d.get = [](const RunConfig & c) {
            return c.moe.cache_auto ? std::string("auto") : std::to_string(c.moe.cache_mb);
        };
        // One value, two fields: a budget and auto-sizing are the two ways to say how big the cache
        // is, so setting either clears the other and the last one given wins.
        d.set = [](RunConfig & c, const std::string & s, std::string & err) {
            if (s == "auto") {
                c.moe.cache_auto = true;
                c.moe.cache_mb = 0;
                return true;
            }
            long long v = 0;
            if (!parse_int(s, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), v, err)) {
                err = "expects a size in MiB or auto, got '" + s + "'";
                return false;
            }
            c.moe.cache_auto = false;
            c.moe.cache_mb = (int) v;
            return true;
        };
        t.push_back(d);
    }
    {
        ParamDesc d = row("cache-floor-mb", "Cache: RAM to leave", G::Cache, L::Advanced,
                          "With the cache on auto: RAM to leave free for the rest of the system.");
        d.value_hint = "N";
        at_least(d, 0, "MiB");
        typical(d, usual::cache_floor_lo_mb, usual::cache_floor_hi_mb);
        bind_int(d, [](RunConfig & c) -> int & { return c.moe.cache_floor_mb; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("cache-ceil-mb", "Cache: ceiling", G::Cache, L::Advanced,
                          "With the cache on auto: upper bound on the budget (0 = no cap).");
        d.value_hint = "N";
        at_least(d, 0, "MiB");
        special(d, "0", "no cap");
        bind_int(d, [](RunConfig & c) -> int & { return c.moe.cache_ceil_mb; });
        t.push_back(d);
    }

    // ── memory ───────────────────────────────────────────────────────────────────────
    {
        ParamDesc d = row("dense-weights", "Dense weights", G::Memory, L::Advanced,
                          "How the dense (non-expert) weights stay resident. mmap leaves them to the kernel; "
                          "warm page-caches them at load (best when the model fits); anon reads them into "
                          "our own buffers so a reclaim hits zram, not flash; ahwb is anon in memory the "
                          "kernel may not reclaim at all, where the platform has such a store.");
        d.type = ParamType::Choice;
        d.value_hint = "mmap|warm|anon|ahwb";
        d.switches = {{"--no-warm-dense", "mmap", true}, {"--dense-odirect", "anon", true}};
        bind_choice<DenseWeightsMode>(d, [](RunConfig & c) -> DenseWeightsMode & { return c.moe.dense_weights; },
                                      {{DenseWeightsMode::Mmap, {"mmap", "mapped from the file"}},
                                       {DenseWeightsMode::Warmed, {"warm", "mapped and read ahead"}},
                                       {DenseWeightsMode::Anonymous, {"anon", "copied into memory"}},
                                       {DenseWeightsMode::Pinned, {"ahwb", "pinned in memory"}}});
        t.push_back(d);
    }
    {
        ParamDesc d = row("ubatch", "Batch width", G::Memory, L::Advanced,
                          "Widest graph computed at once (0 = as wide as the context). Compute buffers are "
                          "reserved for it, so a smaller value hands RAM back to the expert cache at the cost "
                          "of prefill speed; decode is unaffected.");
        d.value_hint = "N";
        bounds(d, 0, RunConfig::tokens_max, "tokens");
        typical(d, usual::ubatch_lo, usual::ubatch_hi);
        special(d, "0", "as wide as the context");
        bind_int(d, [](RunConfig & c) -> int & { return c.n_ubatch; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("row-stream", "Row-stream tables", G::Memory, L::Advanced,
                          "Serve dense tables the graph only gathers rows from (a token embedding) from "
                          "flash instead of RAM. Which tables qualify comes from the graph, not a name list.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--row-stream", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.row_stream; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("row-stream-mb", "Row-stream window", G::Memory, L::Advanced,
                          "Resident window for row-streamed tables.");
        d.value_hint = "N";
        at_least(d, 0, "MiB");
        typical(d, usual::row_stream_lo_mb, usual::row_stream_hi_mb);
        bind_int(d, [](RunConfig & c) -> int & { return c.moe.row_stream_mb; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("gpu-layers", "Layers on a device", G::Memory, L::Advanced,
                          "Layers stored on a compute device, counted from the top as llama.cpp fills them: "
                          "their experts are resident there, not streamed. 0 keeps everything on the host. The "
                          "hardware planner sets it from llama.cpp's own capacity fitter.");
        d.value_hint = "N";
        bounds(d, 0, RunConfig::n_gpu_layers_max, "layers");
        special(d, "0", "all on the host");
        bind_int(d, [](RunConfig & c) -> int & { return c.n_gpu_layers; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("dense-on-device", "Dense weights on device", G::Memory, L::Experimental,
                          "Put the dense weights on an accelerator that shares this host's memory, leaving the "
                          "experts to the streamer. Measured to lose on a phone GPU; kept as a named lever.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--dense-on-device", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.dense_on_device; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("tensor-overrides", "Tensor placement", G::Memory, L::Debug,
                          "The per-tensor placement llama.cpp's capacity fitter wrote: regex patterns over tensor "
                          "names, separated by ';', each kept on the host. Set by the hardware planner; empty "
                          "leaves every tensor where the layer count puts it.");
        d.type = ParamType::Text;
        d.value_hint = "PATTERNS";
        // One string for a list: ';' does not occur in the fitter's patterns, which are plain regexes.
        d.get = [](const RunConfig & c) {
            std::string s;
            for (const std::string & p : c.buft_overrides)
                s += (s.empty() ? "" : ";") + p;
            return s;
        };
        d.set = [](RunConfig & c, const std::string & s, std::string &) {
            c.buft_overrides.clear();
            for (size_t a = 0; !s.empty();) {
                const size_t b = s.find(';', a);
                c.buft_overrides.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
                if (b == std::string::npos) break;
                a = b + 1;
            }
            return true;
        };
        t.push_back(d);
    }
    {
        ParamDesc d = row("device-split", "Layers per device", G::Memory, L::Debug,
                          "How many of the layers on devices each device takes, as NAME:LAYERS pairs separated by "
                          "',', in the order llama.cpp's capacity fitter listed the devices. Set by the hardware "
                          "planner; empty leaves the split to llama.cpp, which divides by free memory.");
        d.type = ParamType::Text;
        d.value_hint = "NAME:N,...";
        d.get = [](const RunConfig & c) {
            std::string s;
            for (const auto & e : c.device_split)
                s += (s.empty() ? "" : ",") + e.first + ":" + std::to_string(e.second);
            return s;
        };
        d.set = [](RunConfig & c, const std::string & s, std::string & err) {
            c.device_split.clear();
            for (size_t a = 0; !s.empty();) {
                const size_t b = s.find(',', a);
                const std::string item = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
                const size_t colon = item.rfind(':');
                char * end = nullptr;
                const long n = colon == std::string::npos ? -1 : std::strtol(item.c_str() + colon + 1, &end, 10);
                if (colon == 0 || n < 1 || n > RunConfig::n_gpu_layers_max || !end || *end) {
                    err = "expected NAME:LAYERS with LAYERS in [1, " + std::to_string(RunConfig::n_gpu_layers_max) +
                          "], got '" + item + "'";
                    return false;
                }
                c.device_split.emplace_back(item.substr(0, colon), (int) n);
                if (b == std::string::npos) break;
                a = b + 1;
            }
            return true;
        };
        t.push_back(d);
    }
    {
        ParamDesc d = row("devices", "Compute devices", G::Memory, L::Advanced,
                          "Which compute devices llama.cpp may use. auto gives it all of them when a layer is "
                          "placed on one and the CPU alone otherwise: a device left registered but unused "
                          "still takes graph nodes, each one a boundary crossing. The prefill device is "
                          "named separately and is not affected.");
        d.type = ParamType::Choice;
        d.value_hint = "auto|all|cpu";
        bind_choice<DeviceUse>(d, [](RunConfig & c) -> DeviceUse & { return c.device_use; },
                               {{DeviceUse::Auto, {"auto", "auto"}},
                                {DeviceUse::All, {"all", "all"}},
                                {DeviceUse::CpuOnly, {"cpu", "CPU only"}}});
        t.push_back(d);
    }

    // ── prefill device ───────────────────────────────────────────────────────────────
    {
        ParamDesc d = row("prefill-device", "Prefill device", G::Prefill, L::Experimental,
                          "Run wide prefill graphs on this ggml device (e.g. HTP0, MTL0) while decode stays on "
                          "the CPU. With streaming the experts reach it through a two-layer arena. Not with "
                          "speculation or row streaming. Empty is off.");
        d.type = ParamType::Text;
        d.value_hint = "D";
        bind_string(d, [](RunConfig & c) -> std::string & { return c.prefill.device; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("prefill-best-effort", "Prefill device is optional", G::Prefill, L::Experimental,
                          "If the prefill device cannot be set up, run the prefill on the CPU instead of failing "
                          "the load. --auto sets it when it chose the device itself.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--prefill-best-effort", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.prefill.best_effort; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("prefill-verify", "Confirm the prefill device", G::Prefill, L::Experimental,
                          "Before keeping a prefill device, prefill a short fixed text on the CPU and on the "
                          "device and keep the device only if it picks the same next token in nearly every "
                          "position. Costs three short prefills at load.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--prefill-verify", "true"}, {"--no-prefill-verify", "false"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.prefill.verify; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("prefill-min-tokens", "Prefill device width", G::Prefill, L::Experimental,
                          "Narrowest prefill piece sent to the prefill device; a shorter tail runs on the CPU.");
        d.value_hint = "N";
        at_least(d, PrefillDeviceConfig::min_tokens_floor, "tokens");
        typical(d, PrefillDeviceConfig::min_tokens_floor, usual::prefill_min_tokens_hi);
        bind_int(d, [](RunConfig & c) -> int & { return c.prefill.min_tokens; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("prefill-slots", "Prefill expert slots", G::Prefill, L::Experimental,
                          "Layers of streamed experts the prefill device holds at once. 2 reads a layer while the "
                          "previous one computes; 1 waits for each layer and needs a layer less of memory.");
        d.value_hint = "N";
        bounds(d, PrefillDeviceConfig::slots_min, PrefillDeviceConfig::slots_max, "layers");
        bind_int(d, [](RunConfig & c) -> int & { return c.prefill.slots; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("prefill-slots-on-demand", "Prefill slots share the cache", G::Prefill, L::Experimental,
                          "Hold the prefill device's expert slots only during a prefill and give their memory to "
                          "the expert cache the rest of the time. The cache keeps its full budget for decode; "
                          "each prefill evicts what the slots need and sets them up again.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--prefill-slots-on-demand", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.prefill.slots_on_demand; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("prefill-loaders", "Prefill loaders", G::Prefill, L::Experimental,
                          "Threads that fill the prefill device's layer slots from flash, with streaming. "
                          "Decode read lanes stay on the read-lane setting.");
        d.value_hint = "N";
        bounds(d, 1, PrefillDeviceConfig::load_threads_max, "threads");
        typical(d, usual::prefill_loaders_lo, usual::prefill_loaders_hi);
        bind_int(d, [](RunConfig & c) -> int & { return c.prefill.load_threads; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("prefill-routed", "Routed prefill reads", G::Prefill, L::Experimental,
                          "With a prefill device and streaming: read only the experts each graph routes to "
                          "(predicted from the previous graph and completed at each routing node) instead of "
                          "every expert of every layer. Output is identical either way.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--prefill-routed", "true"}, {"--no-prefill-routed", "false"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.prefill.routed; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("prefill-routed-full", "Read a busy layer whole", G::Prefill, L::Experimental,
                          "A layer routing more than this fraction of its experts gets the next layer read "
                          "whole. In (0, 1].");
        d.type = ParamType::Float;
        d.value_hint = "F";
        bounds(d, PrefillDeviceConfig::routed_full_frac_min, PrefillDeviceConfig::routed_full_frac_max);
        typical(d, usual::routed_full_lo, usual::routed_full_hi);
        bind_float(d, [](RunConfig & c) -> float & { return c.prefill.routed_full_frac; });
        t.push_back(d);
    }

    // ── decide ───────────────────────────────────────────────────────────────────────
    {
        ParamDesc d = row("decide", "Decide requests", G::Generation, L::Advanced,
                          "With --session: accept decide requests (pick one of a list of choices from a single "
                          "prefill, no decode).");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--decide", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.decide.enabled; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("decide-prefix-cache", "Decide prefix cache", G::Generation, L::Advanced,
                          "With decide: keep the model state after a request's prefix and restore it when the "
                          "next prefix extends it. auto turns it on where prefill cost scales with tokens.");
        d.type = ParamType::Choice;
        d.value_hint = "auto|on|off";
        bind_choice<PrefixCacheMode>(d, [](RunConfig & c) -> PrefixCacheMode & { return c.decide.prefix_cache; },
                                     {{PrefixCacheMode::Auto, {"auto", "auto"}},
                                      {PrefixCacheMode::On, {"on", "on"}},
                                      {PrefixCacheMode::Off, {"off", "off"}}});
        t.push_back(d);
    }
    {
        ParamDesc d = row("decide-probe", "Decide probe", G::Diagnostics, L::Experimental,
                          "With decide: append per decision the experts each layer routed and the answer read "
                          "at every layer's exit (JSONL).");
        d.type = ParamType::Path;
        d.value_hint = "PATH";
        bind_string(d, [](RunConfig & c) -> std::string & { return c.decide.probe_path; });
        t.push_back(d);
    }

    // ── prefetch ─────────────────────────────────────────────────────────────────────
    {
        ParamDesc d = row("prefetch", "Temporal prefetch", G::Prefetch, L::Advanced,
                          "Speculatively read the experts the previous token routed at the next K layers. "
                          "Needs the cache. 0 is off.");
        d.value_hint = "K";
        bounds(d, 0, MoeStreamConfig::prefetch_layers_max, "layers");
        typical(d, usual::prefetch_lo, usual::prefetch_hi);
        special(d, "0", "off");
        bind_int(d, [](RunConfig & c) -> int & { return c.moe.prefetch_layers; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("predict-prefetch", "Predictive prefetch", G::Prefetch, L::Experimental,
                          "Read the experts the next layer's gate predicts on this layer's input, and protect "
                          "predicted residents from eviction. Needs the cache; excludes temporal prefetch.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--predict-prefetch", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.predict_prefetch; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("predict-spec-max", "Predicted misses read", G::Prefetch, L::Experimental,
                          "Predicted misses per layer the predictive prefetch may read (0 = retention only).");
        d.value_hint = "N";
        bounds(d, 0, MoeStreamConfig::predict_spec_max_limit, "reads");
        typical(d, usual::predict_spec_lo, usual::predict_spec_hi);
        special(d, "0", "retention only");
        bind_int(d, [](RunConfig & c) -> int & { return c.moe.predict_spec_max; });
        t.push_back(d);
    }

    // ── speculation ──────────────────────────────────────────────────────────────────
    {
        ParamDesc d = row("spec", "Speculative drafts", G::Speculation, L::Advanced,
                          "Draft a continuation, then verify it in one wider decode. mtp uses the model's own "
                          "prediction head (needs the nextn block); ngram looks the recent tokens up in the "
                          "prompt and the answer. Needs greedy decoding.");
        d.type = ParamType::Choice;
        d.flag.clear();
        d.value_hint = "mtp|ngram";
        d.switches = {{"--mtp", "mtp"}, {"--ngram", "ngram"}};
        d.exclusive_switches = true;
        bind_choice<DraftSource>(d, [](RunConfig & c) -> DraftSource & { return c.spec.source; },
                                 {{DraftSource::none, {"none", "off"}},
                                  {DraftSource::mtp, {"mtp", "MTP head"}},
                                  {DraftSource::ngram, {"ngram", "n-gram lookup"}}});
        t.push_back(d);
    }
    {
        ParamDesc d = row("draft", "Draft width", G::Speculation, L::Advanced, "Tokens drafted per verify batch.");
        d.value_hint = "N";
        bounds(d, 1, SpecConfig::draft_max_limit, "tokens");
        typical(d, usual::draft_lo, usual::draft_hi);
        bind_int(d, [](RunConfig & c) -> int & { return c.spec.draft_max; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("mtp-p-min", "MTP confidence floor", G::Speculation, L::Advanced,
                          "MTP only: stop drafting when the head's best candidate falls below this "
                          "probability (0 = always draft the full width).");
        d.type = ParamType::Float;
        d.value_hint = "F";
        bounds(d, 0.0, SpecConfig::draft_p_min_max);
        special(d, "0", "always the full width");
        bind_float(d, [](RunConfig & c) -> float & { return c.spec.draft_p_min; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("ngram-min-match", "N-gram min match", G::Speculation, L::Advanced,
                          "N-gram only: shortest run of matching tokens allowed to draft.");
        d.value_hint = "N";
        bounds(d, 1, SpecConfig::ngram_match_limit, "tokens");
        typical(d, usual::ngram_min_lo, usual::ngram_min_hi);
        bind_int(d, [](RunConfig & c) -> int & { return c.spec.ngram_min_match; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("ngram-max-match", "N-gram max match", G::Speculation, L::Advanced,
                          "N-gram only: longest suffix considered when looking for a match.");
        d.value_hint = "N";
        bounds(d, 1, SpecConfig::ngram_match_limit, "tokens");
        typical(d, usual::ngram_max_lo, usual::ngram_max_hi);
        bind_int(d, [](RunConfig & c) -> int & { return c.spec.ngram_max_match; });
        t.push_back(d);
    }

    // ── lossy ────────────────────────────────────────────────────────────────────────
    {
        ParamDesc d = row("n-expert-used", "Active experts", G::Lossy, L::Advanced,
                          "Override the experts routed per token (top-k). Lower is faster but changes the "
                          "output. 0 = the model's own value.");
        d.value_hint = "N";
        d.lossy = true;
        bounds(d, 0, RunConfig::n_expert_used_max, "experts");
        special(d, "0", "the model's own");
        bind_int(d, [](RunConfig & c) -> int & { return c.n_expert_used; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("drop-cold-experts", "Drop cold experts", G::Lossy, L::Advanced,
                          "Skip a routed expert that is a cache miss and carries less than F x (1/top-k) of "
                          "the routing's weight. Cache-dependent: changes the output, not reproducibly. "
                          "Needs the cache. 0 is off.");
        d.type = ParamType::Float;
        d.value_hint = "F";
        d.lossy = true;
        bounds(d, 0.0, MoeStreamConfig::drop_cold_frac_max);
        special(d, "0", "off");
        bind_float(d, [](RunConfig & c) -> float & { return c.moe.drop_cold_frac; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("drop-renorm", "Renormalise after drop", G::Lossy, L::Debug,
                          "Rescale the surviving weights after a drop so the routing keeps its mass.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--drop-no-renorm", "false"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.drop_renorm; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("drop-in-prefill", "Drop in prefill", G::Lossy, L::Debug,
                          "Drop during prefill too. Off by default: the cold cache makes it expensive.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--drop-in-prefill", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.drop_prefill; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("expert-substitute", "Expert substitution", G::Lossy, L::Experimental,
                          "Before committing a decode routing, raise every resident expert's score by L x the "
                          "token's score range and re-rank: near-ties resolve toward RAM. Needs the cache. "
                          "0 is off.");
        d.type = ParamType::Float;
        d.value_hint = "L";
        d.lossy = true;
        bounds(d, 0.0, MoeStreamConfig::substitute_lambda_max);
        special(d, "0", "off");
        bind_float(d, [](RunConfig & c) -> float & { return c.moe.substitute_lambda; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("route-ahead", "Route experts early", G::Lossy, L::Experimental,
                          "Commit decode routing to the prediction made N layers earlier in the same forward "
                          "pass, so a prefetch can never miss. Changes the output. Excludes the prefetchers "
                          "and speculation. 0 is off.");
        d.value_hint = "N";
        d.lossy = true;
        bounds(d, 0, MoeStreamConfig::route_ahead_max, "layers");
        typical(d, usual::route_ahead_lo, usual::route_ahead_hi);
        special(d, "0", "off");
        bind_int(d, [](RunConfig & c) -> int & { return c.moe.route_ahead; });
        t.push_back(d);
    }

    // ── diagnostics ──────────────────────────────────────────────────────────────────
    {
        ParamDesc d = row("predict-log", "Prediction probe", G::Diagnostics, L::Debug,
                          "Measure how much of each layer's routing could be known a layer early. Changes "
                          "nothing that is read, but costs a GEMV per layer: not a benchmark run.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--predict-log", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.predict_log; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("llama-debug", "llama.cpp debug log", G::Diagnostics, L::Debug,
                          "Let llama.cpp's own DEBUG log lines through to stderr. Off, they are dropped: about "
                          "1200 lines per model load. For debugging llama.cpp, not the engine.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--llama-debug", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.llama_debug_log; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("load-all", "Load all experts", G::Diagnostics, L::Debug,
                          "Read ALL experts each token: the full-sweep A/B baseline.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--load-all", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.load_all; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("cache-pin", "Pin the cache", G::Cache, L::Advanced,
                          "Keep cached experts in RAM where the platform allows it, so the kernel cannot swap "
                          "or compress them. Pinned memory is taken from everything else on the machine.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--no-cache-pin", "false"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.cache_pin; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("cache-brake", "Brake under pressure", G::Cache, L::Advanced,
                          "Halve the expert cache when the system starts taking it back mid-run: a stalled "
                          "token, confirmed by cache pages no longer in RAM. It only shrinks, never under "
                          "one token's working set, and does nothing where memory is not under pressure.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--no-cache-brake", "false"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.cache_brake; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("cache-pin-fit", "Fit the cache to the lock", G::Cache, L::Advanced,
                          "When the platform stops pinning the cache part way, shrink the budget to what it "
                          "pinned instead of holding the rest as ordinary memory. Worth it where memory "
                          "pressure compresses; --auto sets it when it sized the cache that way.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--cache-pin-fit", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.cache_pin_fit; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("force-cache", "Force cache size", G::Diagnostics, L::Debug,
                          "Allow a cache budget in the pathological band below one token's working set.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--force-cache", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.force_cache; });
        t.push_back(d);
    }
    {
        ParamDesc d = row("prefetch-sync", "Synchronous prefetch", G::Diagnostics, L::Debug,
                          "Tests only: complete each speculative read on the eval thread before returning.");
        d.type = ParamType::Bool;
        d.flag.clear();
        d.switches = {{"--prefetch-sync", "true"}};
        bind_bool(d, [](RunConfig & c) -> bool & { return c.moe.prefetch_sync; });
        t.push_back(d);
    }
    return t;
}

void json_str(std::string & o, const std::string & s) {
    o += '"';
    for (char c : s) {
        switch (c) {
        case '"':
            o += "\\\"";
            break;
        case '\\':
            o += "\\\\";
            break;
        case '\n':
            o += "\\n";
            break;
        case '\r':
            o += "\\r";
            break;
        case '\t':
            o += "\\t";
            break;
        default:
            if ((unsigned char) c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else
                o += c;
        }
    }
    o += '"';
}

// A value in its JSON type: numbers and booleans bare, everything else (paths, choices, "auto") as
// a string, so a form reads `default` without knowing the parameter.
void json_value(std::string & o, const ParamDesc & d, const std::string & v) {
    const bool numeric = (d.type == ParamType::Int || d.type == ParamType::Float) && v != "auto";
    if (d.type == ParamType::Bool || numeric)
        o += v;
    else
        json_str(o, v);
}

void json_num(std::string & o, double v) {
    char b[40];
    std::snprintf(b, sizeof(b), "%.17g", v);
    o += b;
}

} // namespace

bool parse_int_strict(const std::string & s, long long lo, long long hi, long long & out, std::string & err) {
    long long v = 0;
    if (!parse_int(s, std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max(), v, err))
        return false;
    if (v < lo || v > hi) {
        err = "expects an integer in [" + std::to_string(lo) + ", " + std::to_string(hi) + "], got " + s;
        return false;
    }
    out = v;
    return true;
}

const std::vector<ParamDesc> & params() {
    static const std::vector<ParamDesc> table = build();
    return table;
}

const ParamDesc * find_param(const std::string & key) {
    for (const ParamDesc & d : params())
        if (d.key == key) return &d;
    return nullptr;
}

const char * param_type_name(ParamType t) {
    switch (t) {
    case ParamType::Bool:
        return "bool";
    case ParamType::Int:
        return "int";
    case ParamType::Float:
        return "float";
    case ParamType::Choice:
        return "choice";
    case ParamType::Path:
        return "path";
    case ParamType::Text:
        return "text";
    }
    return "?";
}

const char * param_group_name(ParamGroup g) {
    switch (g) {
    case ParamGroup::Model:
        return "model";
    case ParamGroup::Generation:
        return "generation";
    case ParamGroup::Sampling:
        return "sampling";
    case ParamGroup::Streaming:
        return "streaming";
    case ParamGroup::Prefill:
        return "prefill";
    case ParamGroup::Cache:
        return "cache";
    case ParamGroup::Memory:
        return "memory";
    case ParamGroup::Prefetch:
        return "prefetch";
    case ParamGroup::Speculation:
        return "speculation";
    case ParamGroup::Lossy:
        return "lossy";
    case ParamGroup::Diagnostics:
        return "diagnostics";
    }
    return "?";
}

const char * param_group_label(ParamGroup g) {
    switch (g) {
    case ParamGroup::Model:
        return "Model";
    case ParamGroup::Generation:
        return "Generation";
    case ParamGroup::Sampling:
        return "Sampling";
    case ParamGroup::Streaming:
        return "Expert streaming";
    case ParamGroup::Prefill:
        return "Prefill";
    case ParamGroup::Cache:
        return "Expert cache";
    case ParamGroup::Memory:
        return "Memory and residency";
    case ParamGroup::Prefetch:
        return "Prefetch";
    case ParamGroup::Speculation:
        return "Speculative decoding";
    case ParamGroup::Lossy:
        return "Quality trades";
    case ParamGroup::Diagnostics:
        return "Diagnostics";
    }
    return "?";
}

const char * param_group_help(ParamGroup g) {
    switch (g) {
    case ParamGroup::Model:
        return "The model file to run. A front-end with a model list chooses it there.";
    case ParamGroup::Generation:
        return "The answer and what it is computed with: its length, thinking, the context and the threads.";
    case ParamGroup::Sampling:
        return "Greedy and deterministic by default; a temperature above zero turns sampling on.";
    case ParamGroup::Streaming:
        return "How the routed experts are read from storage while a token is decoded.";
    case ParamGroup::Prefill:
        return "How a prompt is computed: on which device, and how its experts are read.";
    case ParamGroup::Cache:
        return "The memory that keeps experts once read, so the next token need not read them again.";
    case ParamGroup::Memory:
        return "Where the weights that are not experts live, and which devices hold layers.";
    case ParamGroup::Prefetch:
        return "Reading experts before a token asks for them.";
    case ParamGroup::Speculation:
        return "Drafting tokens and verifying them in one decode; greedy verification keeps the output identical.";
    case ParamGroup::Lossy:
        return "Faster at the cost of a different reply: each of these changes the output.";
    case ParamGroup::Diagnostics:
        return "Tests, probes and A/B baselines; never a speed setting.";
    }
    return "";
}

bool param_group_read_only(ParamGroup g) {
    return g == ParamGroup::Model;
}

const char * param_level_name(ParamLevel l) {
    switch (l) {
    case ParamLevel::Basic:
        return "basic";
    case ParamLevel::Advanced:
        return "advanced";
    case ParamLevel::Experimental:
        return "experimental";
    case ParamLevel::Debug:
        return "debug";
    }
    return "?";
}

const char * param_scope_name(ParamScope s) {
    return s == ParamScope::Request ? "request" : "session";
}

FlagResult apply_flag(RunConfig & cfg, const char * arg, const char * value) {
    FlagResult r;
    const std::string a = arg ? arg : "";
    for (const ParamDesc & d : params()) {
        if (!d.flag.empty() && (a == d.flag || (!d.short_flag.empty() && a == d.short_flag))) {
            r.matched = true;
            r.param = &d;
            if (!value) {
                r.error = "missing value for " + a;
                return r;
            }
            r.consumed_value = true;
            std::string err;
            if (!d.set(cfg, value, err)) r.error = a + " " + err;
            return r;
        }
        for (const ParamSwitch & s : d.switches) {
            if (a != s.flag) continue;
            r.matched = true;
            r.param = &d;
            r.via_switch = &s;
            std::string err;
            if (!d.set(cfg, s.value, err)) r.error = a + " " + err; // a table bug, caught by the tests
            return r;
        }
    }
    return r;
}

namespace {

// `d` at value `v` as command-line arguments, appended to `out`. A switch that says exactly this
// value is the shortest spelling, and for a switch-only parameter the only one. Appends nothing for
// a value no flag can express (spec=none after --mtp).
void spell(const ParamDesc & d, const std::string & v, std::vector<std::string> & out) {
    const ParamSwitch * sw = nullptr;
    for (const ParamSwitch & s : d.switches)
        if (!s.deprecated && s.value == v) sw = &s;
    if (sw) {
        out.push_back(sw->flag);
    } else if (!d.flag.empty()) {
        out.push_back(d.flag);
        out.push_back(v);
    }
}

} // namespace

std::vector<std::string> to_args(const RunConfig & cfg) {
    const RunConfig base;
    std::vector<std::string> out;
    for (const ParamDesc & d : params()) {
        const std::string v = d.get(cfg);
        if (v == d.get(base)) continue;
        // A value no flag can express is the default, so it cannot differ from base; the case is
        // unreachable and the tests would catch a new one.
        spell(d, v, out);
    }
    return out;
}

std::string flag_spelling(const std::string & key, const std::string & value) {
    const ParamDesc * d = find_param(key);
    if (!d) return "";
    if (value.empty()) return d->flag;
    std::vector<std::string> args;
    spell(*d, value, args);
    std::string out;
    for (const std::string & a : args)
        out += (out.empty() ? "" : " ") + a;
    return out;
}

std::string params_json(const RunConfig & defaults) {
    std::string o = "[";
    bool first = true;
    for (const ParamDesc & d : params()) {
        if (!first) o += ',';
        first = false;
        o += "{\"key\":";
        json_str(o, d.key);
        o += ",\"label\":";
        json_str(o, d.label);
        o += ",\"type\":";
        json_str(o, param_type_name(d.type));
        o += ",\"group\":";
        json_str(o, param_group_name(d.group));
        o += ",\"group_label\":";
        json_str(o, param_group_label(d.group));
        o += ",\"group_help\":";
        json_str(o, param_group_help(d.group));
        o += ",\"group_order\":";
        o += std::to_string((int) d.group);
        if (param_group_read_only(d.group)) o += ",\"group_read_only\":true";
        o += ",\"level\":";
        json_str(o, param_level_name(d.level));
        o += ",\"scope\":";
        json_str(o, param_scope_name(d.scope));
        o += ",\"help\":";
        json_str(o, d.help);
        o += ",\"flag\":";
        json_str(o, d.flag);
        if (!d.short_flag.empty()) {
            o += ",\"short\":";
            json_str(o, d.short_flag);
        }
        if (!d.switches.empty()) {
            o += ",\"switches\":[";
            for (size_t i = 0; i < d.switches.size(); ++i) {
                if (i) o += ',';
                o += "{\"flag\":";
                json_str(o, d.switches[i].flag);
                o += ",\"value\":";
                json_str(o, d.switches[i].value);
                o += d.switches[i].deprecated ? ",\"deprecated\":true}" : "}";
            }
            o += ']';
        }
        if (!d.choices.empty()) {
            o += ",\"choices\":[";
            for (size_t i = 0; i < d.choices.size(); ++i) {
                if (i) o += ',';
                o += "{\"value\":";
                json_str(o, d.choices[i].value);
                o += ",\"label\":";
                json_str(o, d.choices[i].label);
                o += '}';
            }
            o += ']';
        }
        if (d.bounded || d.bounded_below) {
            o += ",\"min\":";
            json_num(o, d.min);
        }
        if (d.bounded) {
            o += ",\"max\":";
            json_num(o, d.max);
        }
        if (d.has_typical) {
            o += ",\"typical\":{\"min\":";
            json_num(o, d.typical_min);
            o += ",\"max\":";
            json_num(o, d.typical_max);
            o += '}';
        }
        if (!d.specials.empty()) {
            o += ",\"special\":[";
            for (size_t i = 0; i < d.specials.size(); ++i) {
                if (i) o += ',';
                o += "{\"value\":";
                json_value(o, d, d.specials[i].value);
                o += ",\"label\":";
                json_str(o, d.specials[i].label);
                o += '}';
            }
            o += ']';
        }
        if (!d.unit.empty()) {
            o += ",\"unit\":";
            json_str(o, d.unit);
        }
        if (d.lossy) o += ",\"lossy\":true";
        if (d.accepts_auto) o += ",\"accepts_auto\":true";
        o += ",\"default\":";
        json_value(o, d, d.get(defaults));
        o += '}';
    }
    o += ']';
    return o;
}

std::string config_json(const RunConfig & cfg) {
    std::string o = "{";
    bool first = true;
    for (const ParamDesc & d : params()) {
        if (!first) o += ',';
        first = false;
        json_str(o, d.key);
        o += ':';
        json_value(o, d, d.get(cfg));
    }
    o += '}';
    return o;
}

} // namespace bmoe
