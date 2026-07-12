#include "bmoe/session.h"
#include "bmoe/recipe.h"
#include "../moe/router_hook.h"
#include "../moe/expert_stream_source.h"
#include "../moe/gguf_offsets.h"

#include "llama.h"
#include "ggml.h"

// llama.cpp's `common` layer (NOT the stable public API): chat-template rendering and
// reasoning parsing. See the note in the root CMakeLists / docs/seam.md.
#include "chat.h"
#include "common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
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

} // namespace

// All native state lives here, behind the pimpl. Built once by Session::open(); every
// generate() reuses the loaded model, the live context and the warm expert cache.
struct Session::Impl {
    SessionConfig cfg;
    std::string arch;
    double load_seconds = 0.0;

    // Ownership order matters at teardown: the source's I/O pool holds fds into the mmap'd
    // file and its buffers back the rebound expert tensors, so it must be shut down before
    // the context and model are freed. The destructor does that explicitly.
    std::unique_ptr<llama_model, void (*)(llama_model *)> model{nullptr, llama_model_free};
    std::unique_ptr<llama_context, void (*)(llama_context *)> ctx{nullptr, llama_free};
    std::unique_ptr<RouterHook> hook; // heap: its address is baked into cparams.cb_eval_user_data
    ExpertStreamSource source;

    const llama_vocab * vocab = nullptr;
    int n_vocab = 0;
    int n_layer = 0;
    bool chat_on = false;
    common_chat_templates_ptr chat_tmpls;
    bool backend_inited = false;

    std::atomic<bool> cancel_requested{false};

    ~Impl() {
        // Deterministic teardown order: stop the I/O pool (it holds fds into the mmap and its
        // buffers back the rebound expert tensors), then the context (its eval callback points
        // at the hook), then the hook, then unmap the model, then release the backend.
        source.shutdown();
        ctx.reset();
        hook.reset();
        model.reset();
        if (backend_inited) llama_backend_free();
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;

double Session::load_seconds() const {
    return impl_->load_seconds;
}
const std::string & Session::arch() const {
    return impl_->arch;
}
int Session::n_ctx() const {
    return impl_->cfg.n_ctx;
}
void Session::cancel() {
    impl_->cancel_requested.store(true, std::memory_order_relaxed);
}

std::unique_ptr<Session> Session::open(const SessionConfig & cfg, std::string & error) {
    auto fail = [&](std::string msg) -> std::unique_ptr<Session> {
        error = std::move(msg);
        return nullptr;
    };

    // Create the session first so its Impl destructor owns backend teardown from this point
    // on: any failure below returns nullptr, destroying `self`, which frees the backend once.
    std::unique_ptr<Session> self(new Session());
    Impl & im = *self->impl_;
    im.cfg = cfg;

    // llama_backend_init/free is process-global and reference counted; init here, free in ~Impl.
    llama_backend_init();
    im.backend_inited = true;

    const auto t_load0 = clock_t_::now();

    // Load with the layout the streamer requires: file-backed mmap, no repack (a repacked
    // q4_K buffer would break the rebind), experts on CPU.
    llama_model_params mparams = llama_model_default_params();
    mparams.use_mmap = true;
    mparams.use_extra_bufts = false;
    mparams.n_gpu_layers = 0;

    llama_model * model = llama_model_load_from_file(cfg.model_path.c_str(), mparams);
    if (!model) return fail("failed to load model: " + cfg.model_path);
    im.model.reset(model);

    im.vocab = llama_model_get_vocab(model);
    im.n_vocab = llama_vocab_n_tokens(im.vocab);
    im.n_layer = llama_model_n_layer(model);

    char arch[128] = {0};
    llama_model_meta_val_str(model, "general.architecture", arch, sizeof(arch));
    im.arch = arch;

    const MoeRecipe * recipe = nullptr;
    if (cfg.moe.enabled) {
        recipe = find_moe_recipe(arch);
        if (!recipe)
            return fail(std::string("no MoE recipe for architecture '") + arch +
                        "' — add one in core/src/moe/arch_registry.cpp (see docs/adding-a-model.md)");
    }

    // Chat templates are model-bound: initialise once here, apply per prompt in generate().
    if (cfg.chatml) {
        try {
            im.chat_tmpls = common_chat_templates_init(model, "");
            im.chat_on = true;
        } catch (const std::exception & e) {
            std::fprintf(stderr, "bmoe: chat template unavailable (%s); using raw prompts\n", e.what());
            im.chat_on = false;
        }
    }

    im.hook = std::make_unique<RouterHook>(recipe ? *recipe : MoeRecipe{}, im.n_layer);
    im.hook->set_prefetch_layers(cfg.moe.prefetch_layers);

    if (cfg.moe.enabled && cfg.moe.spec_gate) {
        if (!recipe->router_input_fmt)
            return fail(std::string("--spec-gate is not supported for architecture '") + arch +
                        "' (no router-input node in its recipe); use --prefetch instead");
        // n_expert_used (top-k) and the router's rms epsilon come from the model metadata — generic
        // arch-prefixed gguf keys, so no per-architecture constants leak into the engine.
        auto meta_int = [&](const std::string & key, int dflt) {
            char buf[64] = {0};
            if (llama_model_meta_val_str(model, key.c_str(), buf, sizeof(buf)) >= 0) return std::atoi(buf);
            return dflt;
        };
        auto meta_float = [&](const std::string & key, float dflt) {
            char buf[64] = {0};
            if (llama_model_meta_val_str(model, key.c_str(), buf, sizeof(buf)) >= 0) return (float) std::atof(buf);
            return dflt;
        };
        const int n_used = meta_int(std::string(arch) + ".expert_used_count", 8);
        const float eps = meta_float(std::string(arch) + ".attention.layer_norm_rms_epsilon", 1e-6f);
        im.hook->set_spec_gate(true, n_used, eps);
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = cfg.n_ctx;
    cparams.n_batch = cfg.n_batch;
    cparams.n_ubatch = cfg.n_batch;
    if (cfg.moe.enabled) {
        cparams.cb_eval = &RouterHook::c_eval;
        cparams.cb_eval_user_data = im.hook.get();
    }

    llama_context * ctx = llama_init_from_model(model, cparams);
    if (!ctx) return fail("failed to create context");
    im.ctx.reset(ctx);
    llama_set_n_threads(ctx, cfg.n_threads, cfg.n_threads);

    // One abort callback for the session's whole life, checking two independent predicates:
    // an explicit cancel() request (any mode) and a fatal streaming I/O error (overlap only).
    // Installing it unconditionally is what lets cancel() interrupt a serial decode too.
    llama_set_abort_callback(
        ctx,
        [](void * ud) -> bool {
            auto * p = static_cast<Impl *>(ud);
            return p->cancel_requested.load(std::memory_order_relaxed) || p->source.fatal();
        },
        &im);

    if (cfg.moe.enabled) {
        // Capture warm-up: one mmap-resident decode so the eval-callback can harvest the expert
        // tensor pointers from the graph. Any valid token builds the same graph (the expert
        // tensor structure is prompt-independent), so use BOS; KV is wiped afterwards.
        im.hook->begin_capture();
        llama_token warm_tok = llama_vocab_bos(im.vocab);
        if (warm_tok < 0) warm_tok = 0;
        llama_batch warm = llama_batch_get_one(&warm_tok, 1);
        if (llama_decode(ctx, warm) != 0) return fail("capture warm-up decode failed");
        im.hook->end_capture();

        GgufOffsets offs = read_gguf_offsets(cfg.model_path.c_str());
        if (!offs.ok) return fail("cannot read gguf offsets: " + cfg.model_path);

        std::vector<LayerExperts> layers = im.hook->captured();
        int n_expert = 0;
        int n_bound = 0;
        for (LayerExperts & L : layers) {
            if (!L.bound) continue;
            ++n_bound;
            for (int p = 0; p < MoeRecipe::max_exps; ++p) {
                if (!recipe->exps_suffix[p]) continue; // slot unused by this architecture
                ggml_tensor * t = L.proj[p].tensor;
                if (!t)
                    return fail(std::string("captured MoE layer is missing expert tensor '") + recipe->exps_suffix[p] +
                                "'");
                auto it = offs.off_by_name.find(t->name);
                if (it == offs.off_by_name.end()) return fail(std::string("no gguf offset for tensor ") + t->name);
                L.proj[p].file_off = it->second;
                const int ne2 = (int) t->ne[2];
                if (n_expert == 0)
                    n_expert = ne2;
                else if (ne2 != n_expert)
                    return fail(std::string("inconsistent expert count: tensor ") + t->name + " has " +
                                std::to_string(ne2) + ", expected " + std::to_string(n_expert));
            }
        }
        if (n_bound == 0) return fail("no MoE expert tensors captured — is this a MoE model?");

        if (!im.source.init(cfg.model_path, n_expert, std::move(layers), cfg.moe))
            return fail("expert stream source init failed");
        im.hook->set_source(&im.source);

        if (cfg.moe.overlap) {
#ifdef BMOE_HAVE_EXPERT_READY_HOOK
            im.source.enable_overlap_hook();
#else
            return fail("--overlap requires the bmoe llama.cpp fork (expert-ready hook not compiled in)");
#endif
        }

        llama_memory_clear(llama_get_memory(ctx), true); // discard warm-up KV
    }

    im.load_seconds = secs(t_load0, clock_t_::now());
    return self;
}

RunResult Session::generate(const GenerateRequest & req,
                            const std::function<void(const TokenMetrics &)> & on_token,
                            IMetricsSink * sink) {
    Impl & im = *impl_;
    const MoeStreamConfig & moe = im.cfg.moe;
    llama_context * ctx = im.ctx.get();

    // Fresh cancel latch for this generation; a stale request from a prior aborted call must
    // not carry over. (cancel() sets it; the abort callback reads it.)
    im.cancel_requested.store(false, std::memory_order_relaxed);

    RunResult res;
    auto fail = [&](std::string msg) {
        res.ok = false;
        res.error = std::move(msg);
        return res;
    };

    if (req.clear_kv) llama_memory_clear(llama_get_memory(ctx), true);

    // Format the prompt. With chat on, render the model's OWN chat template (real Jinja) and
    // set up reasoning parsing so a thinking model's internal reasoning is stripped from the
    // shown answer. req.think drives the template's enable_thinking kwarg, per prompt.
    std::string prompt = req.prompt;
    bool chat_on = im.chat_on;
    common_chat_parser_params parse_params;
    if (chat_on) {
        try {
            common_chat_templates_inputs inputs;
            common_chat_msg user_msg;
            user_msg.role = "user";
            user_msg.content = req.prompt;
            inputs.messages = {user_msg};
            inputs.add_generation_prompt = true;
            inputs.use_jinja = true;
            inputs.enable_thinking = req.think;
            common_chat_params cp = common_chat_templates_apply(im.chat_tmpls.get(), inputs);
            prompt = cp.prompt;
            parse_params = common_chat_parser_params(cp);
            parse_params.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
        } catch (const std::exception & e) {
            std::fprintf(stderr, "bmoe: chat template apply failed (%s); using raw prompt\n", e.what());
            chat_on = false;
        }
    }

    std::vector<llama_token> tokens(prompt.size() + 8);
    int n_prompt = llama_tokenize(im.vocab, prompt.c_str(), (int) prompt.size(), tokens.data(), (int) tokens.size(),
                                  /*add_special*/ true, /*parse_special*/ true);
    if (n_prompt < 0) {
        tokens.resize(-n_prompt);
        n_prompt = llama_tokenize(im.vocab, prompt.c_str(), (int) prompt.size(), tokens.data(), (int) tokens.size(),
                                  true, true);
    }
    if (n_prompt < 1) return fail("empty prompt after tokenization");
    tokens.resize(n_prompt);
    if (n_prompt + req.n_predict + 8 > im.cfg.n_ctx)
        return fail("prompt + n_predict exceeds the session n_ctx (" + std::to_string(im.cfg.n_ctx) +
                    "); open the session with a larger n_ctx");

    // The text to surface: with chat on, parse the raw output so a reasoning model's internal
    // thinking is hidden and only the answer is shown. Generation always uses the raw tokens.
    auto shown_text = [&](const std::string & raw, bool partial) -> std::string {
        if (!chat_on) return raw;
        try {
            return common_chat_parse(raw, partial, parse_params).content;
        } catch (const std::exception &) {
            return raw;
        }
    };

    // ── prefill (chunked by n_batch; positions auto-continue across chunks) ──
    const auto t_prefill0 = clock_t_::now();
    for (int i = 0; i < n_prompt; i += im.cfg.n_batch) {
        const int chunk = std::min(im.cfg.n_batch, n_prompt - i);
        llama_batch pf = llama_batch_get_one(tokens.data() + i, chunk);
        if (llama_decode(ctx, pf) != 0) {
            if (im.cancel_requested.load(std::memory_order_relaxed)) {
                llama_memory_clear(llama_get_memory(ctx), true);
                res.ok = true;
                res.cancelled = true;
                return res;
            }
            if (moe.overlap && im.source.fatal()) return fail("expert stream I/O failed during overlap prefill");
            return fail("prefill decode failed");
        }
    }
    const double prefill_seconds = secs(t_prefill0, clock_t_::now());
    const float * logits = llama_get_logits_ith(ctx, -1);

    // ── greedy generation ──
    res.ok = true;
    std::string gen;
    int n_gen = 0;
    double gen_seconds = 0.0;

    // Baseline snapshot taken AFTER prefill: the summary reports the generation phase only,
    // from real per-token deltas (prefill routes near the whole bank, so folding it into a
    // per-token average would badly inflate the flash-I/O figure). In a warm session these
    // counters carry the prior prompts' totals; the deltas make each prompt self-relative.
    long long prev_bytes = moe.enabled ? (long long) im.source.stats().read_bytes : 0;
    double prev_io_s = moe.enabled ? im.source.stats().read_seconds : 0.0;
    double prev_mgmt_s = moe.enabled ? im.source.stats().mgmt_seconds : 0.0;
    double prev_stall_s = moe.enabled ? im.source.stats().stall_seconds : 0.0;
    long long prev_spec_bytes = moe.enabled ? (long long) im.source.stats().spec_read_bytes : 0;
    long long prev_spec_experts = moe.enabled ? im.source.stats().spec_experts : 0;
    long long prev_spec_useful = moe.enabled ? im.source.stats().spec_useful : 0;
    long long prev_pred_total = im.hook ? im.hook->spec_pred_total() : 0;
    long long prev_pred_hit = im.hook ? im.hook->spec_pred_hit() : 0;

    uint64_t gen_read_bytes = 0;
    double gen_io_seconds = 0.0;
    double gen_mgmt_seconds = 0.0;
    double gen_stall_seconds = 0.0;

    for (int t = 0; t < req.n_predict; ++t) {
        llama_token tok = argmax(logits, im.n_vocab);
        if (llama_vocab_is_eog(im.vocab, tok)) break;

        char piece[256];
        int np = llama_token_to_piece(im.vocab, tok, piece, sizeof(piece), 0, true);
        std::string delta = np > 0 ? std::string(piece, np) : std::string();
        gen += delta;

        auto s0 = clock_t_::now();
        llama_batch step = llama_batch_get_one(&tok, 1);
        int dec = llama_decode(ctx, step);
        auto s1 = clock_t_::now();
        if (dec != 0) {
            if (im.cancel_requested.load(std::memory_order_relaxed)) {
                res.cancelled = true;
                break;
            }
            if (moe.overlap && im.source.fatal()) return fail("expert stream I/O failed during overlap decode");
            return fail("decode failed during generation");
        }
        logits = llama_get_logits_ith(ctx, -1);

        ++n_gen;
        double wall = secs(s0, s1);
        gen_seconds += wall;

        TokenMetrics m;
        m.step = n_gen;
        m.steps = req.n_predict;
        m.wall_ms = wall * 1000.0;
        m.piece = delta;
        m.text = shown_text(gen, /*partial*/ true);
        if (moe.enabled) {
            IExpertSource::Stats st = im.source.stats();
            m.read_bytes = (uint64_t) ((long long) st.read_bytes - prev_bytes);
            m.io_ms = (st.read_seconds - prev_io_s) * 1000.0;
            m.mgmt_ms = (st.mgmt_seconds - prev_mgmt_s) * 1000.0;
            if (moe.overlap) {
                m.stall_ms = (st.stall_seconds - prev_stall_s) * 1000.0 / im.cfg.n_threads;
                m.compute_ms = m.wall_ms - m.stall_ms - m.mgmt_ms;
            } else {
                m.compute_ms = m.wall_ms - m.io_ms - m.mgmt_ms;
            }
            if (m.compute_ms < 0) m.compute_ms = 0;
            m.cache_hit_pct = st.cache_lookups > 0 ? 100.0 * st.cache_hits / st.cache_lookups : -1.0;
            prev_bytes = (long long) st.read_bytes;
            prev_io_s = st.read_seconds;
            prev_mgmt_s = st.mgmt_seconds;
            prev_stall_s = st.stall_seconds;
            gen_read_bytes += m.read_bytes;
            gen_io_seconds += m.io_ms / 1000.0;
            gen_mgmt_seconds += m.mgmt_ms / 1000.0;
            gen_stall_seconds += m.stall_ms / 1000.0;
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
    s.n_prompt = n_prompt;
    s.load_seconds = im.load_seconds;
    s.prefill_seconds = prefill_seconds;
    if (moe.enabled) {
        IExpertSource::Stats st = im.source.stats();
        s.moe_read_mib = gen_read_bytes / (1024.0 * 1024.0);
        s.moe_io_seconds = gen_io_seconds;
        s.moe_io_s_per_token = n_gen ? gen_io_seconds / n_gen : 0.0;
        s.moe_mgmt_s_per_token = n_gen ? gen_mgmt_seconds / n_gen : 0.0;
        s.moe_stall_s_per_token = n_gen ? gen_stall_seconds / n_gen : 0.0;
        s.moe_compute_s_per_token =
            s.s_per_token - (moe.overlap ? s.moe_stall_s_per_token : s.moe_io_s_per_token) - s.moe_mgmt_s_per_token;
        if (s.moe_compute_s_per_token < 0) s.moe_compute_s_per_token = 0;
        s.cache_hit_pct = st.cache_lookups > 0 ? 100.0 * st.cache_hits / st.cache_lookups : -1.0;
        s.cache_resident_mib = st.cache_resident_bytes / (1024.0 * 1024.0);
        s.moe_spec_read_mib = ((long long) st.spec_read_bytes - prev_spec_bytes) / (1024.0 * 1024.0);
        s.moe_spec_experts = st.spec_experts - prev_spec_experts;
        s.moe_spec_useful = st.spec_useful - prev_spec_useful;
        if (moe.spec_gate && im.hook) {
            const long long pt = im.hook->spec_pred_total() - prev_pred_total;
            const long long ph = im.hook->spec_pred_hit() - prev_pred_hit;
            s.moe_spec_recall_pct = pt > 0 ? 100.0 * ph / pt : -1.0;
        }
    }
    if (sink) sink->on_summary(s);

    res.generated_text = shown_text(gen, /*partial*/ false);

    // A cancel left the KV cache holding a partial generation; clear it so the next prompt
    // (which clears anyway when clear_kv=true) never continues from stale state.
    if (res.cancelled) llama_memory_clear(llama_get_memory(ctx), true);
    return res;
}

} // namespace bmoe
