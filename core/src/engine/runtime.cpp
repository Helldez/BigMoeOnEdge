#include "bmoe/runtime.h"
#include "bmoe/recipe.h"
#include "../moe/router_hook.h"
#include "../moe/expert_stream_source.h"
#include "../moe/gguf_offsets.h"

#include "llama.h"
#include "ggml.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace bmoe {

namespace {

using clock_t_ = std::chrono::steady_clock;
double secs(clock_t_::time_point a, clock_t_::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

llama_token argmax(const float * logits, int n_vocab) {
    llama_token best = 0;
    float best_v = logits[0];
    for (int v = 1; v < n_vocab; ++v)
        if (logits[v] > best_v) {
            best_v = logits[v];
            best = v;
        }
    return best;
}

std::string apply_chatml(const std::string & user) {
    return "<|im_start|>user\n" + user + "<|im_end|>\n<|im_start|>assistant\n";
}

RunResult fail(std::string msg) {
    RunResult r;
    r.ok = false;
    r.error = std::move(msg);
    return r;
}

} // namespace

RunResult run(const RunConfig & cfg, const std::function<void(const TokenMetrics &)> & on_token, IMetricsSink * sink) {
    ValidationResult v = validate(cfg);
    if (!v) return fail(v.error);

    llama_backend_init();
    struct BackendGuard {
        ~BackendGuard() { llama_backend_free(); }
    } backend_guard;

    // Load with the layout the streamer requires: file-backed mmap, no repack (a repacked
    // q4_K buffer would break the rebind), experts on CPU.
    llama_model_params mparams = llama_model_default_params();
    mparams.use_mmap = true;
    mparams.use_extra_bufts = false;
    mparams.n_gpu_layers = 0;

    llama_model * model = llama_model_load_from_file(cfg.model_path.c_str(), mparams);
    if (!model) return fail("failed to load model: " + cfg.model_path);
    std::unique_ptr<llama_model, void (*)(llama_model *)> model_guard(model, llama_model_free);

    const llama_vocab * vocab = llama_model_get_vocab(model);
    const int n_vocab = llama_vocab_n_tokens(vocab);
    const int n_layer = llama_model_n_layer(model);

    const MoeRecipe * recipe = nullptr;
    if (cfg.moe.enabled) {
        char arch[128] = {0};
        llama_model_meta_val_str(model, "general.architecture", arch, sizeof(arch));
        recipe = find_moe_recipe(arch);
        if (!recipe)
            return fail(std::string("no MoE recipe for architecture '") + arch +
                        "' — add one in core/src/moe/arch_registry.cpp (see docs/adding-a-model.md)");
    }

    // Tokenize the (optionally ChatML-wrapped) prompt.
    const std::string prompt = cfg.chatml ? apply_chatml(cfg.prompt) : cfg.prompt;
    std::vector<llama_token> tokens(prompt.size() + 8);
    int n_prompt = llama_tokenize(vocab, prompt.c_str(), (int) prompt.size(), tokens.data(), (int) tokens.size(),
                                  /*add_special*/ true, /*parse_special*/ true);
    if (n_prompt < 0) {
        tokens.resize(-n_prompt);
        n_prompt =
            llama_tokenize(vocab, prompt.c_str(), (int) prompt.size(), tokens.data(), (int) tokens.size(), true, true);
    }
    if (n_prompt < 1) return fail("empty prompt after tokenization");
    tokens.resize(n_prompt);

    int n_ctx = cfg.n_ctx;
    if (n_ctx < n_prompt + cfg.n_predict + 8) n_ctx = n_prompt + cfg.n_predict + 8;

    RouterHook hook(recipe ? *recipe : MoeRecipe{}, n_layer);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = n_ctx;
    cparams.n_batch = n_prompt;
    cparams.n_ubatch = n_prompt;
    if (cfg.moe.enabled) {
        cparams.cb_eval = &RouterHook::c_eval;
        cparams.cb_eval_user_data = &hook;
    }

    llama_context * ctx = llama_init_from_model(model, cparams);
    if (!ctx) return fail("failed to create context");
    std::unique_ptr<llama_context, void (*)(llama_context *)> ctx_guard(ctx, llama_free);
    llama_set_n_threads(ctx, cfg.n_threads, cfg.n_threads);

    ExpertStreamSource source;

    if (cfg.moe.enabled) {
        // Capture warm-up: one mmap-resident decode so the eval-callback can harvest the
        // expert tensor pointers from the graph. KV is wiped afterwards.
        hook.begin_capture();
        llama_token first = tokens[0];
        llama_batch warm = llama_batch_get_one(&first, 1);
        if (llama_decode(ctx, warm) != 0) return fail("capture warm-up decode failed");
        hook.end_capture();

        // Resolve gguf file offsets by tensor name and count experts.
        GgufOffsets offs = read_gguf_offsets(cfg.model_path.c_str());
        if (!offs.ok) return fail("cannot read gguf offsets: " + cfg.model_path);

        std::vector<LayerExperts> layers = hook.captured();
        int n_expert = 0;
        int n_bound = 0;
        for (LayerExperts & L : layers) {
            if (!L.bound) continue;
            ++n_bound;
            for (int p = 0; p < 3; ++p) {
                ggml_tensor * t = L.proj[p].tensor;
                if (!t) return fail("captured layer is missing a projection tensor");
                auto it = offs.off_by_name.find(t->name);
                if (it == offs.off_by_name.end()) return fail(std::string("no gguf offset for tensor ") + t->name);
                L.proj[p].file_off = it->second;
                if (n_expert == 0) n_expert = (int) t->ne[2];
            }
        }
        if (n_bound == 0) return fail("no MoE expert tensors captured — is this a MoE model?");

        if (!source.init(cfg.model_path, n_expert, std::move(layers), cfg.moe))
            return fail("expert stream source init failed");
        hook.set_source(&source);

        llama_memory_clear(llama_get_memory(ctx), true); // discard warm-up KV
    }

    // ── prefill ──
    {
        llama_batch pf = llama_batch_get_one(tokens.data(), n_prompt);
        if (llama_decode(ctx, pf) != 0) return fail("prefill decode failed");
    }
    const float * logits = llama_get_logits_ith(ctx, -1); // logits of the last output

    // ── greedy generation ──
    RunResult res;
    res.ok = true;
    std::string gen;
    int n_gen = 0;
    double gen_seconds = 0.0;

    // Baseline snapshot taken AFTER prefill: the summary reports the generation phase
    // only, from real per-token deltas, never a total divided by n_gen. Prefill routes
    // the union of the prompt's experts (near the whole bank), so folding it into a
    // per-token average would badly inflate the flash-I/O figure.
    long long prev_bytes = cfg.moe.enabled ? (long long) source.stats().read_bytes : 0;
    double prev_io_s = cfg.moe.enabled ? source.stats().read_seconds : 0.0;

    // Generation-only accumulators, summed from the measured per-token values.
    uint64_t gen_read_bytes = 0;
    double gen_io_seconds = 0.0;

    for (int t = 0; t < cfg.n_predict; ++t) {
        llama_token tok = argmax(logits, n_vocab);
        if (llama_vocab_is_eog(vocab, tok)) break;

        char piece[256];
        int np = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, true);
        std::string delta = np > 0 ? std::string(piece, np) : std::string();
        gen += delta;

        auto s0 = clock_t_::now();
        llama_batch step = llama_batch_get_one(&tok, 1);
        int dec = llama_decode(ctx, step);
        auto s1 = clock_t_::now();
        if (dec != 0) return fail("decode failed during generation");
        logits = llama_get_logits_ith(ctx, -1);

        ++n_gen;
        double wall = secs(s0, s1);
        gen_seconds += wall;

        TokenMetrics m;
        m.step = n_gen;
        m.steps = cfg.n_predict;
        m.wall_ms = wall * 1000.0;
        m.piece = delta;
        m.text = gen;
        if (cfg.moe.enabled) {
            IExpertSource::Stats st = source.stats();
            m.read_bytes = (uint64_t) ((long long) st.read_bytes - prev_bytes);
            m.io_ms = (st.read_seconds - prev_io_s) * 1000.0;
            m.compute_ms = m.wall_ms - m.io_ms;
            if (m.compute_ms < 0) m.compute_ms = 0;
            m.cache_hit_pct = st.cache_lookups > 0 ? 100.0 * st.cache_hits / st.cache_lookups : -1.0;
            prev_bytes = (long long) st.read_bytes;
            prev_io_s = st.read_seconds;
            gen_read_bytes += m.read_bytes;
            gen_io_seconds += m.io_ms / 1000.0;
        } else {
            m.compute_ms = m.wall_ms;
            m.cache_hit_pct = -1.0;
        }
        if (on_token) on_token(m);
        if (sink) sink->on_token(m);
    }

    // ── summary ──
    RunSummary & s = res.summary;
    s.n_generated = n_gen;
    s.gen_seconds = gen_seconds;
    s.s_per_token = n_gen ? gen_seconds / n_gen : 0.0;
    s.tokens_per_second = gen_seconds > 0 ? n_gen / gen_seconds : 0.0;
    if (cfg.moe.enabled) {
        IExpertSource::Stats st = source.stats();
        // Generation-phase figures, summed from the measured per-token deltas (prefill
        // excluded) — real measurements, not a contaminated total divided by n_gen.
        s.moe_read_mib = gen_read_bytes / (1024.0 * 1024.0);
        s.moe_io_seconds = gen_io_seconds;
        s.moe_io_s_per_token = n_gen ? gen_io_seconds / n_gen : 0.0;
        s.moe_compute_s_per_token = s.s_per_token - s.moe_io_s_per_token;
        if (s.moe_compute_s_per_token < 0) s.moe_compute_s_per_token = 0;
        s.cache_hit_pct = st.cache_lookups > 0 ? 100.0 * st.cache_hits / st.cache_lookups : -1.0;
        s.cache_resident_mib = st.cache_resident_bytes / (1024.0 * 1024.0);
    }
    if (sink) sink->on_summary(s);

    res.generated_text = std::move(gen);

    // Tear the source's I/O pool down before the context/model guards fire.
    source.shutdown();
    return res;
}

} // namespace bmoe
