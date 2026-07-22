#include "bmoe/session.h"
#include "bmoe/recipe.h"
#include "bmoe/route_trace.h"
#include "bmoe/decode_trace.h"
#include "chat_parse.h"
#include "thinking_control.h"
#include "../moe/router_hook.h"
#include "../moe/expert_stream_source.h"
#include "../moe/gguf_offsets.h"
#include "../io/platform_io.h"

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
#include <unordered_set>
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

// The generation phase's running measurement state, and the one place a generated token's cost is
// written down. The cursors hold the source's absolute totals as of the previous token — its stats
// are cumulative across a warm session, so every per-token flash figure is a delta against these —
// and the totals are what the summary averages over n_gen. Grouped into one object so the per-token
// metrics block can be a method instead of ten more locals threaded through generate().
struct GenTally {
    // Fixed for the run; kept here so record() needs only the token's own measurements.
    bool overlap = false;
    int n_threads = 1;

    long long prev_bytes = 0;
    double prev_io_s = 0.0;
    double prev_mgmt_s = 0.0;
    double prev_stall_s = 0.0;

    uint64_t read_bytes = 0;
    double io_seconds = 0.0;
    double mgmt_seconds = 0.0;
    double stall_seconds = 0.0;
    uint64_t majflt = 0;
    double cpu_seconds = 0.0;

    // Fill in everything a generated token is measured by — its wall/fault/CPU decomposition, the
    // memory picture, and (when streaming) the flash figures taken as deltas against the cursors
    // above — then advance those cursors and the run totals. The token's TEXT stays with the
    // caller: what a token says depends on chat state, what it cost does not.
    //
    // `wall` is the decode's wall time in seconds and `faults`/`cpu_s` the deltas measured around
    // that same decode; `st` is the expert source's stats, or null when streaming is off.
    void
    record(TokenMetrics & m, double wall, uint64_t faults, double cpu_s, int turn, const IExpertSource::Stats * st) {
        m.wall_ms = wall * 1000.0;
        // Fault/CPU decomposition is independent of streaming — dense-weight faults show up in the
        // mmap baseline too — so record it for every token before the moe/no-moe split below.
        m.majflt = faults;
        m.cpu_ms = cpu_s * 1000.0;
        m.majflt_mib = (double) m.majflt * (double) pio::fault_bytes() / (1024.0 * 1024.0);
        m.turn = turn;
        majflt += m.majflt;
        cpu_seconds += cpu_s;

        // Read the memory picture AFTER the decode, outside the caller's timing bracket: two /proc
        // reads are cheap but they are not this token's work, and billing them to wall_ms would
        // corrupt the very number the reader is here to trust.
        pio::ProcessMemory pm;
        if (pio::process_memory(&pm)) {
            m.rss_mib = pm.rss_bytes / (1024.0 * 1024.0);
            m.rss_anon_mib = pm.rss_anon_bytes / (1024.0 * 1024.0);
            m.rss_file_mib = pm.rss_file_bytes / (1024.0 * 1024.0);
            m.swap_mib = pm.swap_bytes / (1024.0 * 1024.0);
        }
        pio::DeviceMemory dm;
        if (pio::device_memory(&dm)) {
            m.mem_available_mib = dm.available_bytes / (1024.0 * 1024.0);
            m.mem_free_mib = dm.free_bytes / (1024.0 * 1024.0);
            m.swap_free_mib = dm.swap_free_bytes / (1024.0 * 1024.0);
        }
        if (!st) {
            m.compute_ms = m.wall_ms;
            m.cache_hit_pct = -1.0;
            return;
        }

        m.dense_resident_frac = st->dense_resident_frac;
        m.cache_budget_mib = st->cache_budget_bytes / (1024.0 * 1024.0);
        m.read_bytes = (uint64_t) ((long long) st->read_bytes - prev_bytes);
        m.io_ms = (st->read_seconds - prev_io_s) * 1000.0;
        m.mgmt_ms = (st->mgmt_seconds - prev_mgmt_s) * 1000.0;
        if (overlap) {
            m.stall_ms = (st->stall_seconds - prev_stall_s) * 1000.0 / n_threads;
            m.compute_ms = m.wall_ms - m.stall_ms - m.mgmt_ms;
        } else {
            m.compute_ms = m.wall_ms - m.io_ms - m.mgmt_ms;
        }
        if (m.compute_ms < 0) m.compute_ms = 0;
        m.cache_hit_pct = st->cache_lookups > 0 ? 100.0 * st->cache_hits / st->cache_lookups : -1.0;

        prev_bytes = (long long) st->read_bytes;
        prev_io_s = st->read_seconds;
        prev_mgmt_s = st->mgmt_seconds;
        prev_stall_s = st->stall_seconds;
        read_bytes += m.read_bytes;
        io_seconds += m.io_ms / 1000.0;
        mgmt_seconds += m.mgmt_ms / 1000.0;
        stall_seconds += m.stall_ms / 1000.0;
    }
};

// The names of the expert weight tensors the streamer rebinds. "Dense" is defined by subtraction —
// everything the model has that is NOT one of these — so both consumers below start by asking this
// same question, and used to answer it with their own copy of the same triple loop.
std::unordered_set<std::string> expert_tensor_names(const std::vector<LayerExperts> & layers) {
    std::unordered_set<std::string> names;
    for (const LayerExperts & L : layers) {
        if (!L.bound) continue;
        for (int p = 0; p < MoeRecipe::max_exps; ++p)
            if (L.proj[p].tensor) names.insert(L.proj[p].tensor->name);
    }
    return names;
}

// Each layer's bytes that the streamer does NOT manage: everything under blk.<il>. except the
// expert weight tensors it rebinds — attention, norms, the router, and any per-expert scale left
// mmap-resident. This is what the layer costs to page in, and it is a static property of the
// file: nothing about decoding changes it, which is why the route trace states it once in the
// static block instead of pretending to measure it per step.
std::vector<uint64_t>
dense_bytes_per_layer(const GgufOffsets & offs, const std::vector<LayerExperts> & layers, int n_layer) {
    const std::unordered_set<std::string> streamed = expert_tensor_names(layers);
    std::vector<uint64_t> out((size_t) std::max(0, n_layer), 0);
    for (const auto & kv : offs.size_by_name) {
        int il = -1;
        if (std::sscanf(kv.first.c_str(), "blk.%d.", &il) != 1) continue;
        if (il < 0 || il >= n_layer || streamed.count(kv.first)) continue;
        out[(size_t) il] += kv.second;
    }
    return out;
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
    // How a think=false request can be honoured on this model; probed once at open(). Template
    // (the fail-open default) means the flag alone does the job and generate() adds nothing.
    ThinkControl think_ctl = ThinkControl::Template;
    bool backend_inited = false;

    // Sampling chain, built once at open() only when sampling is requested (temp > 0); null on the
    // greedy default, where the decode loop stays on the argmax fast path. See open()/generate().
    llama_sampler * smpl = nullptr;

    // Multi-turn chat state (chat mode only). chat_history is the running conversation the
    // template is re-rendered over each turn; kv_tokens mirrors the tokens currently decoded
    // into the context's KV (seq 0), in order, so the next turn can reuse the common prefix
    // and prefill only the diverging suffix instead of re-running the whole conversation.
    std::vector<common_chat_msg> chat_history;
    std::vector<llama_token> kv_tokens;

    // Route trace (diagnostics): null unless requested AND streaming is on — there is no routing
    // to trace otherwise.
    IRouteTraceSink * route_trace = nullptr;

    // Which generate() this is, 0-based. It labels a trace's rows, and it labels every token's
    // metrics — a multi-turn CSV is unreadable without it, and the two-turn A/B (a fast turn, an
    // idle, then the turn that pays for it) is exactly what this engine is measured by.
    int turn = 0;

    // Decode traces (diagnostics): null unless requested. The compute trace needs no streaming —
    // it measures the graph, which a dense mmap run has too; the I/O trace needs the streamer,
    // since there are no engine-issued reads without it. See bmoe/decode_trace.h.
    IComputeTraceSink * compute_trace = nullptr;
    IIoTraceSink * io_trace = nullptr;
    std::vector<IoTraceRow> io_rows_scratch;

    // Stated once to a metrics sink, before the first token: the model and configuration every row
    // it writes was produced under. info_sent guards a session's many generate() calls from
    // interleaving preambles between turns.
    RunInfo info;
    bool info_sent = false;

    std::atomic<bool> cancel_requested{false};

    ~Impl() {
        // Deterministic teardown order: stop the I/O pool (it holds fds into the mmap and its
        // buffers back the rebound expert tensors), then the context (its eval callback points
        // at the hook), then the hook, then unmap the model, then release the backend.
        source.shutdown();
        if (smpl) llama_sampler_free(smpl); // independent of ctx/model; free before them
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
ThinkControl Session::think_control() const {
    return impl_->think_ctl;
}
void Session::set_cache_budget_mb(int mib) {
    impl_->source.set_cache_budget((size_t) std::max(0, mib) * 1024ull * 1024ull);
}
void Session::cancel() {
    impl_->cancel_requested.store(true, std::memory_order_relaxed);
}

std::unique_ptr<Session> Session::open(const SessionConfig & cfg,
                                       std::string & error,
                                       IRouteTraceSink * route_trace,
                                       IComputeTraceSink * compute_trace,
                                       IIoTraceSink * io_trace) {
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

    // The gguf header answers three separate questions below — the arch-prefixed key for a top-k
    // override, the route trace's effective top-k, and the run info's top-k/expert count — and each
    // used to reopen and reparse the file for its own answer. Read it at most once, lazily: the
    // callers are conditional (a run with no override and no trace asks nothing), so an eager read
    // would be work the common path never needs.
    GgufModelInfo gguf_info;
    bool gguf_info_read = false;
    auto gguf = [&]() -> const GgufModelInfo & {
        if (!gguf_info_read) {
            gguf_info = read_gguf_model_info(cfg.model_path.c_str());
            gguf_info_read = true;
        }
        return gguf_info;
    };

    // Load with the layout the streamer requires: file-backed mmap, no repack (a repacked
    // q4_K buffer would break the rebind), experts on CPU.
    llama_model_params mparams = llama_model_default_params();
    mparams.use_mmap = true;
    mparams.use_extra_bufts = false;
    mparams.n_gpu_layers = 0;

    // Optional active-expert override: reduce the model's top-k routing (e.g. 8 -> 6) to cut
    // per-token compute and — under streaming — flash I/O, at a quality cost. Applied purely
    // through llama.cpp's public kv_overrides on the arch-prefixed expert_used_count key: the
    // graph then routes to fewer experts and the whole streaming path adapts automatically
    // (the router hook reads the top-k width from the graph). The array must outlive the load
    // call below. See docs/adding-a-model.md — no llama.cpp patch, no per-arch constants.
    llama_model_kv_override kv_overrides[2];
    std::memset(kv_overrides, 0, sizeof(kv_overrides)); // second entry stays the key[0]==0 terminator
    if (cfg.n_expert_used > 0) {
        const GgufModelInfo & info = gguf();
        if (!info.ok) return fail("cannot read gguf metadata: " + cfg.model_path);
        if (info.arch.empty()) return fail("gguf has no general.architecture; cannot set n_expert_used");
        if (info.n_expert <= 0)
            return fail("n_expert_used was set but the model is not MoE (no " + info.arch + ".expert_count)");
        if (cfg.n_expert_used > info.n_expert)
            return fail("n_expert_used=" + std::to_string(cfg.n_expert_used) + " exceeds the model's expert count (" +
                        std::to_string(info.n_expert) + ")");
        const std::string key = info.arch + ".expert_used_count";
        kv_overrides[0].tag = LLAMA_KV_OVERRIDE_TYPE_INT;
        std::snprintf(kv_overrides[0].key, sizeof(kv_overrides[0].key), "%s", key.c_str());
        kv_overrides[0].val_i64 = cfg.n_expert_used;
        mparams.kv_overrides = kv_overrides;
    }

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
            // Which "thinking off" mechanism this template supports is a property of the model, so
            // it is settled once here rather than re-derived on every turn.
            im.think_ctl = detail::probe_think_control(im.chat_tmpls.get());
        } catch (const std::exception & e) {
            std::fprintf(stderr, "bmoe: chat template unavailable (%s); using raw prompts\n", e.what());
            im.chat_on = false;
        }
    }

    im.hook = std::make_unique<RouterHook>(recipe ? *recipe : MoeRecipe{}, im.n_layer);
    im.hook->set_prefetch_layers(cfg.moe.prefetch_layers);
    im.hook->set_drop_policy(cfg.moe.drop_cold_frac, cfg.moe.drop_renorm, cfg.moe.drop_prefill);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = cfg.n_ctx;
    cparams.n_batch = cfg.n_batch;
    cparams.n_ubatch = cfg.n_batch;
    // The streamer needs the callback to see routing; the compute trace needs it to time nodes.
    // Installing it for the trace alone is what lets a NON-streamed run be measured — the dense
    // mmap baseline the streamed numbers are argued against.
    if (cfg.moe.enabled || compute_trace) {
        cparams.cb_eval = &RouterHook::c_eval;
        cparams.cb_eval_user_data = im.hook.get();
    }

    llama_context * ctx = llama_init_from_model(model, cparams);
    if (!ctx) return fail("failed to create context");
    im.ctx.reset(ctx);
    llama_set_n_threads(ctx, cfg.n_threads, cfg.n_threads);

    // Opt-in sampling. temp <= 0 leaves smpl null and the decode loop on argmax — the deterministic
    // default the byte-identity gates rely on. temp > 0 builds the standard chain, using only the
    // public llama_sampler_* API (hard rule 1): common_sampler lives in the non-stable common layer.
    static_assert(SamplingConfig{}.seed == LLAMA_DEFAULT_SEED,
                  "SamplingConfig::seed default must mirror LLAMA_DEFAULT_SEED");
    if (cfg.sampling.temp > 0.0f) {
        im.smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(im.smpl, llama_sampler_init_top_k(cfg.sampling.top_k));
        llama_sampler_chain_add(im.smpl, llama_sampler_init_top_p(cfg.sampling.top_p, /*min_keep*/ 1));
        llama_sampler_chain_add(im.smpl, llama_sampler_init_temp(cfg.sampling.temp));
        llama_sampler_chain_add(im.smpl, llama_sampler_init_dist(cfg.sampling.seed));
    }

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

        // Computed before init consumes `layers`: the dense split needs the captured expert
        // tensor names and the gguf sizes together.
        std::vector<uint64_t> dense_bytes;
        if (route_trace) dense_bytes = dense_bytes_per_layer(offs, layers, im.n_layer);

        // Anonymous dense-weights mode: hand the streamer the dense (non-expert) model weights to
        // read into anon buffers. The list is every captured weight leaf that IS a gguf tensor
        // (dropping graph inputs and KV, which share the leaf shape) and is NOT one of the streamed
        // experts. Built before init consumes `layers`. Only this mode needs them; the others ignore
        // an empty list.
        if (cfg.moe.dense_weights == DenseWeightsMode::Anonymous || cfg.moe.dense_weights == DenseWeightsMode::Pinned) {
            const std::unordered_set<std::string> expert_names = expert_tensor_names(layers);
            std::vector<DenseTensorRef> dense;
            for (const auto & kv : im.hook->captured_weights()) {
                const std::string & name = kv.first;
                if (expert_names.count(name)) continue;
                auto off = offs.off_by_name.find(name);
                auto sz = offs.size_by_name.find(name);
                if (off == offs.off_by_name.end() || sz == offs.size_by_name.end()) continue; // not a file tensor
                DenseTensorRef d;
                d.tensor = kv.second;
                d.file_off = off->second;
                d.size = sz->second;
                dense.push_back(d);
            }
            im.source.set_dense_tensors(std::move(dense));
        }

        if (!im.source.init(cfg.model_path, n_expert, std::move(layers), cfg.moe))
            return fail("expert stream source init failed");
        im.hook->set_source(&im.source);

        if (route_trace) {
            im.route_trace = route_trace;
            im.hook->set_trace(true);

            RouteTraceStatic st;
            st.model = cfg.model_path;
            st.arch = im.arch;
            st.n_layer = im.n_layer;
            st.n_expert = n_expert;
            // The effective top-k: an override IS the applied width, otherwise the model's own.
            st.n_expert_used = cfg.n_expert_used;
            if (st.n_expert_used <= 0) {
                const GgufModelInfo & info = gguf();
                st.n_expert_used = info.ok ? info.n_expert_used : 0;
            }
            st.dense_bytes_per_layer = std::move(dense_bytes);
            st.expert_bytes_per_layer.resize((size_t) im.n_layer);
            for (int il = 0; il < im.n_layer; ++il)
                st.expert_bytes_per_layer[(size_t) il] = im.source.expert_bytes(il);
            route_trace->on_static(st);
        }

        if (io_trace) {
            im.io_trace = io_trace;
            im.source.set_io_trace(true);
        }

        if (cfg.moe.overlap) {
#ifdef BMOE_HAVE_EXPERT_READY_HOOK
            im.source.enable_overlap_hook();
#else
            return fail("--overlap requires the bmoe llama.cpp fork (expert-ready hook not compiled in)");
#endif
        }

        llama_memory_clear(llama_get_memory(ctx), true); // discard warm-up KV
    }

    // Decode traces. Outside the streaming block on purpose: the compute trace measures the graph,
    // which exists with or without the streamer, so a dense mmap baseline can be traced and
    // compared. The I/O trace was armed above (it needs the source) and only reports here.
    if (compute_trace || im.io_trace) {
        DecodeTraceStatic st;
        st.model = cfg.model_path;
        st.arch = im.arch;
        st.n_layer = im.n_layer;
        st.n_threads = cfg.n_threads;
        st.io_threads = cfg.moe.enabled ? cfg.moe.io_threads : 0;
        st.o_direct = cfg.moe.enabled && cfg.moe.o_direct;
        st.overlap = cfg.moe.enabled && cfg.moe.overlap;
        if (compute_trace) {
            im.compute_trace = compute_trace;
            im.hook->set_compute_trace(true, cfg.compute_trace_layers);
            compute_trace->on_static(st);
        }
        if (im.io_trace) im.io_trace->on_static(st);
    }

    // What this session IS, for any metrics sink that will describe what it DOES. Built here, where
    // every fact is resolved: cache_mb in particular is what the streamer settled on, which under
    // auto-sizing is a number no flag ever mentioned.
    {
        RunInfo & ri = im.info;
        const std::string & p = cfg.model_path;
        const size_t slash = p.find_last_of("/\\");
        ri.model = slash == std::string::npos ? p : p.substr(slash + 1);
        ri.arch = im.arch;
        ri.n_layer = im.n_layer;
        ri.n_threads = cfg.n_threads;
        ri.n_ctx = cfg.n_ctx;
        ri.moe_stream = cfg.moe.enabled;
        ri.cache_auto = cfg.moe.cache_auto;
        ri.cache_ceil_mb = cfg.moe.cache_ceil_mb;
        ri.force_cache = cfg.moe.force_cache;
        ri.io_threads = cfg.moe.enabled ? cfg.moe.io_threads : 0;
        ri.o_direct = cfg.moe.enabled && cfg.moe.o_direct;
        ri.overlap = cfg.moe.enabled && cfg.moe.overlap;
        ri.prefetch_layers = cfg.moe.enabled ? cfg.moe.prefetch_layers : 0;
        ri.drop_cold_frac = cfg.moe.enabled ? cfg.moe.drop_cold_frac : 0.0f;
        // The CSV keeps the two familiar flags, derived from the resolved dense-weights policy.
        ri.dense_weights = cfg.moe.dense_weights == DenseWeightsMode::Mmap        ? "mmap"
                           : cfg.moe.dense_weights == DenseWeightsMode::Anonymous ? "anon"
                           : cfg.moe.dense_weights == DenseWeightsMode::Pinned    ? "ahwb"
                                                                                  : "warm";
        if (cfg.moe.enabled) {
            const IExpertSource::Stats st = im.source.stats();
            ri.cache_mb = (int) (st.cache_budget_bytes / (1024ull * 1024ull));
        }
        // The EFFECTIVE top-k: an override IS the applied width, otherwise the model's own. Same
        // resolution the route trace does, and worth a header read — a run whose top-k is unknown
        // cannot be compared against one whose top-k differs, which is most of the point.
        ri.n_expert_used = cfg.n_expert_used;
        if (ri.n_expert_used <= 0 || ri.n_expert == 0) {
            const GgufModelInfo & mi = gguf();
            if (mi.ok) {
                ri.n_expert = mi.n_expert;
                if (ri.n_expert_used <= 0) ri.n_expert_used = mi.n_expert_used;
            }
        }
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

    // Before anything is written about what this run did, say what it was.
    if (sink && !im.info_sent) {
        sink->on_run_info(im.info);
        im.info_sent = true;
    }

    // Fresh cancel latch for this generation; a stale request from a prior aborted call must
    // not carry over. (cancel() sets it; the abort callback reads it.)
    im.cancel_requested.store(false, std::memory_order_relaxed);

    RunResult res;
    auto fail = [&](std::string msg) {
        res.ok = false;
        res.error = std::move(msg);
        return res;
    };

    // clear_kv = "new chat": drop the KV and the engine-held conversation. Otherwise this turn
    // continues the conversation, reusing the KV prefix already decoded from earlier turns.
    if (req.clear_kv) {
        llama_memory_clear(llama_get_memory(ctx), true);
        im.chat_history.clear();
        im.kv_tokens.clear();
        // A new chat resets the sampler RNG, so a fixed seed reproduces the same transcript from a
        // fresh conversation. A continued turn (clear_kv=false) keeps the stream going, matching the
        // KV it decodes against.
        if (im.smpl) llama_sampler_reset(im.smpl);
    }

    // Format the prompt. With chat on, render the model's OWN chat template (real Jinja) over the
    // WHOLE conversation so far, and set up reasoning parsing so a thinking model's internal
    // reasoning is stripped from the shown answer. req.think drives enable_thinking, per prompt.
    std::string prompt = req.prompt;
    bool chat_on = im.chat_on;
    bool history_pushed = false;   // did we append this turn's user message to chat_history?
    bool prefilled_answer = false; // closed the reasoning span in the prompt, so skip reasoning parse
    common_chat_parser_params parse_params;
    if (chat_on) {
        try {
            common_chat_msg user_msg;
            user_msg.role = "user";
            user_msg.content = req.prompt;
            im.chat_history.push_back(user_msg);
            history_pushed = true;

            common_chat_templates_inputs inputs;
            inputs.messages = im.chat_history; // the full conversation, not just this turn
            inputs.add_generation_prompt = true;
            inputs.use_jinja = true;
            inputs.enable_thinking = req.think;
            // AUTO is what bakes reasoning-stripping into the generated parser grammar. It is set
            // here, before apply — the field defaults to NONE, which produces a content-only
            // grammar that leaves <think> markers in the answer no matter how the parse is wired.
            inputs.reasoning_format = COMMON_REASONING_FORMAT_AUTO;

            // Many templates never read enable_thinking (LFM2.5 among them): the flag reaches the
            // jinja context, is discarded, and the model reasons anyway — the setting silently does
            // nothing. For those, close the reasoning span in the prompt instead, so the model
            // resumes at the first token of its answer with the reasoning already behind it.
            //
            // The span is rendered by llama.cpp's own handler for this template, so no marker for
            // any family — harmony's primed final channel included — is spelled out here. Which
            // models need this was measured at open(), not assumed. See thinking_control.h.
            if (!req.think && im.think_ctl == ThinkControl::Prefill) {
                detail::add_no_think_prefill(inputs);
                prefilled_answer = true;
            }

            common_chat_params cp = common_chat_templates_apply(im.chat_tmpls.get(), inputs);
            prompt = cp.prompt;
            parse_params = detail::build_parse_params(cp);
        } catch (const std::exception & e) {
            std::fprintf(stderr, "bmoe: chat template apply failed (%s); using raw prompt\n", e.what());
            if (history_pushed) {
                im.chat_history.pop_back();
                history_pushed = false;
            }
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
    // thinking is separated from the answer. The answer is shown inline; the reasoning is handed to
    // the UI as a distinct thinking block rather than dropped, so a Thinking-on run does not sit on a
    // blank screen while the model reasons. Generation always uses the raw tokens.
    struct ShownView {
        std::string content;   // the answer, reasoning stripped
        std::string reasoning; // the thinking span, empty unless the parser split one out
    };
    auto shown_view = [&](const std::string & raw, bool partial) -> ShownView {
        if (!chat_on) return {raw, ""};
        // A prefilled turn resumes mid-answer: the reasoning span and the turn header the parser
        // anchors on are already in the prompt, not in the stream, so there is nothing to strip —
        // the raw stream already IS the answer. (If a model reasons anyway despite the closed span,
        // that reasoning surfaces verbatim rather than being cut out here. Hiding it would only
        // disguise a model this mechanism does not work on; the honest report is ThinkControl::None.)
        if (prefilled_answer) return {raw, ""};
        try {
            common_chat_msg msg = common_chat_parse(raw, partial, parse_params);
            return {msg.content, msg.reasoning_content};
        } catch (const std::exception & e) {
            detail::warn_parse_failed_once(e.what());
            return {raw, ""};
        }
    };

    // Reuse the KV prefix already decoded from earlier turns (chat mode only): find how many
    // leading tokens still match the cache, drop the divergent tail, and prefill only the suffix.
    // Keeping at least one token to decode means a turn is never a no-op. clear_kv leaves
    // kv_tokens empty, so n_common = 0 and this reduces to a full prefill — the one-shot path the
    // byte-identity gates exercise stays unchanged.
    size_t n_common = 0;
    if (chat_on && !im.kv_tokens.empty()) {
        const size_t max_common = tokens.size() > 0 ? tokens.size() - 1 : 0;
        while (n_common < im.kv_tokens.size() && n_common < max_common && im.kv_tokens[n_common] == tokens[n_common])
            ++n_common;
        if (n_common < im.kv_tokens.size()) {
            // SWA-style memory (e.g. Gemma) can refuse a partial removal; fall back to a full
            // re-prefill in that case rather than continuing from an inconsistent cache.
            if (!llama_memory_seq_rm(llama_get_memory(ctx), 0, (llama_pos) n_common, -1)) {
                llama_memory_clear(llama_get_memory(ctx), true);
                n_common = 0;
            }
            im.kv_tokens.resize(n_common);
        }
    }

    // Roll this turn back to the state before it started: drop the KV added this turn, forget the
    // tokens we fed, and un-append the user message. Used on cancel so prior turns stay usable.
    auto rollback_turn = [&]() {
        if (chat_on) {
            if (!llama_memory_seq_rm(llama_get_memory(ctx), 0, (llama_pos) n_common, -1))
                llama_memory_clear(llama_get_memory(ctx), true);
            im.kv_tokens.resize(n_common);
            if (history_pushed) {
                im.chat_history.pop_back();
                history_pushed = false;
            }
        } else {
            llama_memory_clear(llama_get_memory(ctx), true);
        }
    };

    // Route trace: frame one decode's rows, then hand them to the sink once it has returned —
    // never from inside the callback, which runs on a compute thread mid-graph. `base_pos` is
    // the context position of the batch's first token, so prefill rows carry real step numbers.
    // The frame the I/O rows are stamped with at flush; the other traces carry their own.
    int trace_phase = 0, trace_step = 0;
    auto trace_begin = [&](int base_pos, int n_tokens, int phase) {
        // Not a trace concern, but the same per-decode frame: the drop policy is decode-only
        // unless armed for prefill, so it has to be told which phase this batch is.
        im.hook->set_batch_phase(phase);
        if (im.route_trace) im.hook->begin_trace_batch(base_pos, n_tokens, phase, im.turn);
        // A node is computed once for the whole batch, not per token, so a prefill chunk's graph is
        // attributed to its last position rather than pretending to split across the chunk.
        if (im.compute_trace) im.hook->begin_compute_batch(base_pos + n_tokens - 1, phase, im.turn);
        trace_phase = phase;
        trace_step = base_pos + n_tokens - 1;
    };
    auto trace_flush = [&]() {
        // Close the compute trace's dangling interval FIRST: at layer granularity the "post" row
        // is charged the wall since the last boundary, and everything trace_flush does before the
        // close would be billed to the LM head.
        if (im.compute_trace) im.hook->end_compute_batch();
        if (im.route_trace) {
            im.hook->end_trace_batch();
            std::vector<RouteTraceRow> & rows = im.hook->trace_rows();
            if (!rows.empty()) im.route_trace->on_rows(rows.data(), rows.size());
            rows.clear();
        }
        if (im.compute_trace) {
            std::vector<ComputeTraceRow> & rows = im.hook->compute_rows();
            if (!rows.empty()) im.compute_trace->on_rows(rows.data(), rows.size());
            rows.clear();
        }
        if (im.io_trace) {
            // The reads carry no frame of their own — a lane does not know which token it serves —
            // so stamp them with the decode they were drained after.
            im.source.take_io_trace_rows(im.io_rows_scratch);
            for (IoTraceRow & r : im.io_rows_scratch) {
                r.turn = im.turn;
                r.phase = trace_phase;
                r.step = trace_step;
            }
            if (!im.io_rows_scratch.empty()) im.io_trace->on_rows(im.io_rows_scratch.data(), im.io_rows_scratch.size());
            im.io_rows_scratch.clear();
        }
    };

    // ── prefill (chunked by n_batch; positions auto-continue from the reused prefix) ──
    const auto t_prefill0 = clock_t_::now();
    for (int i = (int) n_common; i < n_prompt; i += im.cfg.n_batch) {
        const int chunk = std::min(im.cfg.n_batch, n_prompt - i);
        llama_batch pf = llama_batch_get_one(tokens.data() + i, chunk);
        trace_begin(i, chunk, /*phase*/ 0);
        if (llama_decode(ctx, pf) != 0) {
            if (im.cancel_requested.load(std::memory_order_relaxed)) {
                rollback_turn();
                res.ok = true;
                res.cancelled = true;
                return res;
            }
            if (moe.overlap && im.source.fatal()) return fail("expert stream I/O failed during overlap prefill");
            return fail("prefill decode failed");
        }
        trace_flush();
    }
    // The suffix is now in the KV; record it so the next turn can diff against it.
    if (chat_on)
        for (int i = (int) n_common; i < n_prompt; ++i)
            im.kv_tokens.push_back(tokens[i]);
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
    GenTally tally;
    tally.overlap = moe.overlap;
    tally.n_threads = im.cfg.n_threads;
    if (moe.enabled) {
        const IExpertSource::Stats st0 = im.source.stats();
        tally.prev_bytes = (long long) st0.read_bytes;
        tally.prev_io_s = st0.read_seconds;
        tally.prev_mgmt_s = st0.mgmt_seconds;
        tally.prev_stall_s = st0.stall_seconds;
    }
    long long prev_spec_bytes = moe.enabled ? (long long) im.source.stats().spec_read_bytes : 0;
    long long prev_spec_experts = moe.enabled ? im.source.stats().spec_experts : 0;
    long long prev_spec_useful = moe.enabled ? im.source.stats().spec_useful : 0;
    // Taken after prefill, so the drop counters describe generation — the phase the policy is armed
    // for and the one the tok/s number is about.
    const long long prev_routed = im.hook->experts_routed();
    const long long prev_dropped = im.hook->experts_dropped();

    for (int t = 0; t < req.n_predict; ++t) {
        // Greedy stays argmax (byte-identical to the resident reference the gates check); with a
        // sampling chain, draw from the context's last-position logits, which llama_sampler_sample
        // reads at index -1 — the same logits argmax would have read.
        llama_token tok = im.smpl ? llama_sampler_sample(im.smpl, ctx, -1) : argmax(logits, im.n_vocab);
        if (llama_vocab_is_eog(im.vocab, tok)) break;

        char piece[256];
        int np = llama_token_to_piece(im.vocab, tok, piece, sizeof(piece), 0, true);
        std::string delta = np > 0 ? std::string(piece, np) : std::string();
        gen += delta;

        // Bracket ONLY the decode: major faults and CPU-time deltas here decompose this token's
        // compute residual into flash-fault stalls vs. genuine (or throttled) computation.
        const uint64_t f0 = pio::major_faults();
        const double c0 = pio::process_cpu_seconds();
        auto s0 = clock_t_::now();
        llama_batch step = llama_batch_get_one(&tok, 1);
        trace_begin(n_prompt + t, /*n_tokens*/ 1, /*phase*/ 1); // decodes at the position after the prompt
        int dec = llama_decode(ctx, step);
        auto s1 = clock_t_::now();
        const uint64_t f1 = pio::major_faults();
        const double c1 = pio::process_cpu_seconds();
        if (dec != 0) {
            if (im.cancel_requested.load(std::memory_order_relaxed)) {
                res.cancelled = true;
                break;
            }
            if (moe.overlap && im.source.fatal()) return fail("expert stream I/O failed during overlap decode");
            return fail("decode failed during generation");
        }
        trace_flush(); // outside the s0..s1 bracket: the trace's own writes must not bill wall_ms
        logits = llama_get_logits_ith(ctx, -1);
        if (chat_on) im.kv_tokens.push_back(tok); // this token is now in the KV

        ++n_gen;
        double wall = secs(s0, s1);
        gen_seconds += wall;

        TokenMetrics m;
        m.step = n_gen;
        m.steps = req.n_predict;
        m.piece = delta;
        {
            ShownView sv = shown_view(gen, /*partial*/ true);
            m.text = std::move(sv.content);
            m.reasoning = std::move(sv.reasoning);
        }
        const IExpertSource::Stats st = moe.enabled ? im.source.stats() : IExpertSource::Stats{};
        tally.record(m, wall, f1 - f0, c1 - c0, im.turn, moe.enabled ? &st : nullptr);
        if (on_token) on_token(m);
        if (sink) sink->on_token(m);
    }

    ++im.turn; // this turn is written; label the next one apart

    // ── summary ──
    RunSummary & s = res.summary;
    s.n_generated = n_gen;
    s.gen_seconds = gen_seconds;
    s.s_per_token = n_gen ? gen_seconds / n_gen : 0.0;
    s.tokens_per_second = gen_seconds > 0 ? n_gen / gen_seconds : 0.0;
    s.n_prompt = n_prompt - (int) n_common; // tokens actually prefilled this turn (after KV reuse)
    s.n_past = chat_on ? (int) im.kv_tokens.size() : n_prompt + n_gen; // total context length now
    s.load_seconds = im.load_seconds;
    s.prefill_seconds = prefill_seconds;
    s.majflt_per_token = n_gen ? (double) tally.majflt / n_gen : 0.0;
    s.cpu_s_per_token = n_gen ? tally.cpu_seconds / n_gen : 0.0;
    if (moe.enabled) {
        IExpertSource::Stats st = im.source.stats();
        s.moe_read_mib = tally.read_bytes / (1024.0 * 1024.0);
        s.moe_io_seconds = tally.io_seconds;
        s.moe_io_s_per_token = n_gen ? tally.io_seconds / n_gen : 0.0;
        s.moe_mgmt_s_per_token = n_gen ? tally.mgmt_seconds / n_gen : 0.0;
        s.moe_stall_s_per_token = n_gen ? tally.stall_seconds / n_gen : 0.0;
        s.moe_compute_s_per_token =
            s.s_per_token - (moe.overlap ? s.moe_stall_s_per_token : s.moe_io_s_per_token) - s.moe_mgmt_s_per_token;
        if (s.moe_compute_s_per_token < 0) s.moe_compute_s_per_token = 0;
        s.cache_hit_pct = st.cache_lookups > 0 ? 100.0 * st.cache_hits / st.cache_lookups : -1.0;
        s.cache_resident_mib = st.cache_resident_bytes / (1024.0 * 1024.0);
        s.cache_budget_mib = st.cache_budget_bytes / (1024.0 * 1024.0);
        s.cache_resizes = st.cache_resizes;
        s.token_demand_mib = st.token_demand_bytes / (1024.0 * 1024.0);
        s.layer_demand_mib = st.layer_demand_bytes / (1024.0 * 1024.0);
        s.moe_spec_read_mib = ((long long) st.spec_read_bytes - prev_spec_bytes) / (1024.0 * 1024.0);
        s.moe_spec_experts = st.spec_experts - prev_spec_experts;
        s.moe_spec_useful = st.spec_useful - prev_spec_useful;
    }
    s.experts_routed = im.hook->experts_routed() - prev_routed;
    s.experts_dropped = im.hook->experts_dropped() - prev_dropped;
    if (sink) sink->on_summary(s);

    {
        ShownView sv = shown_view(gen, /*partial*/ false);
        res.generated_text = std::move(sv.content);
        res.reasoning_text = std::move(sv.reasoning);
    }

    if (res.cancelled) {
        // Undo the whole turn (KV, fed tokens, and the pushed user message) so the conversation
        // is left exactly as it was before this prompt and stays continuable.
        rollback_turn();
    } else if (chat_on) {
        // Commit the assistant turn to the running conversation. Parsing separates a thinking
        // model's reasoning from the answer; the next turn re-renders history from these messages.
        common_chat_msg assistant;
        if (prefilled_answer) {
            // Prefilled turn: no turn header in the stream to parse; the generation is the answer
            // verbatim. It is committed without the prefill, so history holds a normal assistant
            // message and the next turn re-renders cleanly whatever this turn's think setting was.
            assistant.content = gen;
        } else {
            try {
                assistant = common_chat_parse(gen, /*is_partial*/ false, parse_params);
            } catch (const std::exception & e) {
                detail::warn_parse_failed_once(e.what());
                assistant.content = gen;
            }
        }
        assistant.role = "assistant";
        im.chat_history.push_back(assistant);
    }
    return res;
}

} // namespace bmoe
