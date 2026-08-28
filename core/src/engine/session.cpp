#include "bmoe/session.h"
#include "bmoe/recipe.h"
#include "bmoe/route_trace.h"
#include "bmoe/decode_trace.h"
#include "bmoe/version.h"
#include "bmoe/ngram_draft.h"
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
#include "speculative.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

// Fill an explicitly-allocated batch with `n` tokens at consecutive positions on sequence 0.
//
// The engine otherwise decodes through llama_batch_get_one, which leaves pos/seq_id/logits null and
// lets llama.cpp infer them. Speculation cannot: the driver reads the batch's sequence ids, and a
// verify pass needs logits at EVERY position, not just the last. So every batch on the speculative
// path is spelled out — including prefill, which the driver must see to keep the draft context's
// KV in step with the target's.
void batch_fill(llama_batch & b, const llama_token * toks, int n, llama_pos pos0, bool all_logits) {
    b.n_tokens = n;
    for (int i = 0; i < n; ++i) {
        b.token[i] = toks[i];
        b.pos[i] = pos0 + i;
        b.n_seq_id[i] = 1;
        b.seq_id[i][0] = 0;
        b.logits[i] = (int8_t) (all_logits || i == n - 1);
    }
}

// Graph width for the MTP draft context, and it wants to be SMALL.
//
// llama.cpp reserves a context's compute buffers for its widest ubatch, and the dominant term is the
// output buffer, which scales with ubatch x vocabulary — at 256 on a 152k-vocab model that alone is
// ~156 MB, inside a reservation measured at 493 MiB on device. The draft context never needs it:
// at decode time it evaluates ONE token per draft step, the catch-up hands it at most 1 + draft_max
// positions, and that catch-up asks for no logits at all. Only prefill ever feeds it a wide batch,
// and that is one layer, so splitting it into more ubatches costs very little.
//
// The width was 256 until the device A/B showed what it cost. Half a gigabyte of reservation is what
// tipped the phone past its memory budget: major faults per token went from ~69 without speculation
// to 632 at draft 3, as the kernel swapped and dropped file pages to find the room. On this engine
// memory is never free — it is the expert cache's, and the cache is what decides whether the wider
// verify read set is a hit or a flash read.
//
// Raised to 1 + draft_max when a very wide draft asks for more, and clamped down to the target's own
// ubatch so it is never the wider of the two.
constexpr int mtp_draft_ubatch = 32;

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

    long long prev_bytes = 0;
    double prev_io_s = 0.0;
    double prev_mgmt_s = 0.0;
    double prev_stall_s = 0.0;

    uint64_t read_bytes = 0;
    double io_seconds = 0.0;
    double mgmt_seconds = 0.0;
    double stall_seconds = 0.0;
    double drain_seconds = 0.0;
    double adopt_seconds = 0.0;
    double prev_drain_s = 0.0;
    double prev_adopt_s = 0.0;
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
            // stall is already wall-additive (the union of stalled intervals), so no thread-count
            // normalization: dividing summed thread time by n_threads was a mean that understated
            // the stall whenever a minority of threads did the waiting, and the difference quietly
            // became "compute".
            m.stall_ms = (st->stall_seconds - prev_stall_s) * 1000.0;
            m.compute_ms = m.wall_ms - m.stall_ms - m.mgmt_ms;
        } else {
            m.compute_ms = m.wall_ms - m.io_ms - m.mgmt_ms;
        }
        if (m.compute_ms < 0) m.compute_ms = 0;
        m.cache_hit_pct = st->cache_lookups > 0 ? 100.0 * st->cache_hits / st->cache_lookups : -1.0;
        m.drain_ms = (st->drain_wait_seconds - prev_drain_s) * 1000.0;
        m.adopt_ms = (st->adopt_wait_seconds - prev_adopt_s) * 1000.0;

        prev_bytes = (long long) st->read_bytes;
        prev_io_s = st->read_seconds;
        prev_mgmt_s = st->mgmt_seconds;
        prev_stall_s = st->stall_seconds;
        prev_drain_s = st->drain_wait_seconds;
        prev_adopt_s = st->adopt_wait_seconds;
        read_bytes += m.read_bytes;
        io_seconds += m.io_ms / 1000.0;
        mgmt_seconds += m.mgmt_ms / 1000.0;
        stall_seconds += m.stall_ms / 1000.0;
        drain_seconds += m.drain_ms / 1000.0;
        adopt_seconds += m.adopt_ms / 1000.0;
    }
};

// The prompt phase's measurement, the prefill counterpart of GenTally above: the source's
// counters are cumulative across a warm session, so begin() pins them (with the process CPU
// clock) just above the prompt chunk loop and end() closes the deltas just after it — the same
// wall-additive terms the decode fields report, read with the same rules. end()'s sample is the
// phase boundary itself: the decode baseline below seeds its cursors from it, so prefill's end
// and decode's start are one reading of the counters, not two that could drift apart.
struct PrefillTally {
    IExpertSource::Stats pre;
    double cpu0 = 0.0;

    // Deltas across this turn's prefill chunks — valid after end().
    double cpu_seconds = 0.0;
    double read_mib = 0.0;
    double io_seconds = 0.0;
    double stall_seconds = 0.0;
    double mgmt_seconds = 0.0;

    // The stats sample end() closed on, for the decode baseline to start from.
    IExpertSource::Stats post;

    void begin(bool moe_on, const IExpertSource & src) {
        pre = moe_on ? src.stats() : IExpertSource::Stats{};
        cpu0 = pio::process_cpu_seconds();
    }
    void end(bool moe_on, const IExpertSource & src) {
        post = moe_on ? src.stats() : IExpertSource::Stats{};
        cpu_seconds = pio::process_cpu_seconds() - cpu0;
        read_mib = (double) ((long long) post.read_bytes - (long long) pre.read_bytes) / (1024.0 * 1024.0);
        io_seconds = post.read_seconds - pre.read_seconds;
        stall_seconds = post.stall_seconds - pre.stall_seconds;
        mgmt_seconds = post.mgmt_seconds - pre.mgmt_seconds;
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

    // The MTP draft source (SpecConfig::source == mtp). A SECOND context over the SAME model,
    // created with ctx_type = MTP so llama.cpp builds the nextn graph instead of the trunk one. It
    // carries the same eval callback as the target — the hook is per-context, not per-model — so the
    // MTP block's expert layer is captured and streamed by the one source both contexts share.
    //
    // The n-gram source has no equivalent state: it drafts from the token history alone, which is
    // why it costs no context, no memory and no expert read. Everything below this pair is shared by
    // both sources, because the verify half of the loop does not care who drafted.
    std::unique_ptr<llama_context, void (*)(llama_context *)> ctx_dft{nullptr, llama_free};
    common_speculative_ptr mtp;
    // Serves both roles on the speculative path, never both at once: a prefill chunk (up to
    // n_batch tokens, logits on the last) and a verify pass (1 + draft_max tokens, logits on all).
    llama_batch mtp_batch{};
    bool mtp_batch_owned = false; // whether mtp_batch holds a llama_batch_init allocation to free
    std::vector<llama_token> draft_buf;
    long long mtp_drafted = 0;
    long long mtp_accepted = 0;
    long long mtp_decodes = 0;
    // Steps that drafted anything at all. Against mtp_decodes this is the n-gram source's match
    // rate — the fraction of the run where it had evidence and widened the verify batch. For the
    // head it is all of them, since it drafts unconditionally unless p_min stops it.
    long long drafted_steps = 0;
    // Time spent drafting and catching the draft context up, i.e. everything speculation adds
    // OUTSIDE the target decode. It used to land in loop_overhead_ms together with sampling and
    // rendering, where it could only be inferred by differencing against an unspeculated run.
    double mtp_draft_seconds = 0.0;
    // Flash bytes those passes streamed. The MTP block is a MoE layer of its own, so drafting has
    // an I/O cost as well as a compute one — and it is invisible to the route trace, whose framing
    // brackets the target decode only. Without this the growth in bytes/token under speculation
    // cannot be split between the widened verify union on the trunk and the head's own routing,
    // which are attacked in completely different ways.
    uint64_t mtp_draft_read_bytes = 0;

    const llama_vocab * vocab = nullptr;
    int n_vocab = 0;
    int n_expert_used = 0; // effective routing width, after any override (0 = not MoE)
    int n_layer = 0;
    // Trained MTP heads in this gguf (0 = no nextn block). The MTP block occupies layer indices
    // [n_layer, n_layer + n_layer_nextn), contiguous with the trunk and using the same tensor
    // naming, so widening the streamer's layer bound by it is all the streamer needs to reach it.
    int n_layer_nextn = 0;
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
        // The speculative driver holds both contexts and detaches the backend samplers it
        // installed on the draft one, so it goes before either context is freed.
        mtp.reset();
        if (mtp_batch_owned) llama_batch_free(mtp_batch);
        ctx_dft.reset();
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
int Session::n_expert_used() const {
    return impl_->n_expert_used;
}
ThinkControl Session::think_control() const {
    return impl_->think_ctl;
}
void Session::set_cache_budget_mb(int mib) {
    impl_->source.set_cache_budget((size_t) std::max(0, mib) * 1024ull * 1024ull);
}
// Teacher-forced perplexity. See PplRequest for why the batch is labelled decode.
PplResult Session::perplexity(const PplRequest & req) {
    auto & im = *impl_;
    PplResult r;
    const auto t0 = clock_t_::now();
    llama_context * ctx = im.ctx.get();

    std::vector<llama_token> tokens(req.text.size() + 8);
    int n = llama_tokenize(im.vocab, req.text.c_str(), (int) req.text.size(), tokens.data(), (int) tokens.size(),
                           /*add_special*/ true, /*parse_special*/ false);
    if (n < 0) {
        tokens.resize(-n);
        n = llama_tokenize(im.vocab, req.text.c_str(), (int) req.text.size(), tokens.data(), (int) tokens.size(), true,
                           false);
    }
    if (n < 2) {
        r.error = "text too short to score";
        return r;
    }
    tokens.resize(n);
    if (n > im.cfg.n_ctx) {
        r.error =
            "text of " + std::to_string(n) + " tokens exceeds the session n_ctx (" + std::to_string(im.cfg.n_ctx) + ")";
        return r;
    }
    r.n_tokens = n;

    llama_memory_clear(llama_get_memory(ctx), true);
    im.kv_tokens.clear();

    // Warm-up graph, discarded. Both routing policies decide at the terminal node of each layer's
    // weight chain, and which node that is gets LEARNED from the first graph of a run — so on a
    // single-batch pass they would never fire at all, and every cell would score the baseline. One
    // throwaway decode teaches the hook the shape; the scoring pass then runs with the policy live.
    {
        llama_batch warm = llama_batch_init(1, 0, 1);
        batch_fill(warm, tokens.data(), 1, 0, false);
        im.hook->set_batch_phase(req.as_decode ? 1 : 0);
        const int rc = llama_decode(ctx, warm);
        llama_batch_free(warm);
        if (rc != 0) {
            r.error = "warm-up decode failed";
            return r;
        }
        llama_memory_clear(llama_get_memory(ctx), true);
    }
    const long long routed0 = im.hook->experts_routed();
    const long long dropped0 = im.hook->experts_dropped();
    const long long reranked0 = im.hook->experts_reranked();
    const long long substituted0 = im.hook->experts_substituted();

    // Logits at EVERY position, so one pass scores every token — llama_batch_get_one would ask for
    // the last position only.
    llama_batch b = llama_batch_init(im.cfg.n_batch, /*embd*/ 0, /*n_seq_max*/ 1);
    struct BatchGuard {
        llama_batch & b;
        ~BatchGuard() { llama_batch_free(b); }
    } guard{b};

    double nll = 0.0;
    int scored = 0, top1 = 0;
    int last_row = 0; // logits row of the text's final position, for the choices
    // Score the logits at row `row` of the last decode against the token at `pos + 1`.
    auto score = [&](int row, int pos) -> bool {
        const float * lg = llama_get_logits_ith(ctx, row);
        if (!lg) {
            r.error = "no logits at position " + std::to_string(pos);
            return false;
        }
        // log softmax at the token that actually follows, in a numerically safe order.
        float max = lg[0];
        for (int v = 1; v < im.n_vocab; ++v)
            if (lg[v] > max) max = lg[v];
        double sum = 0.0;
        for (int v = 0; v < im.n_vocab; ++v)
            sum += std::exp((double) (lg[v] - max));
        const double logp = (double) (lg[tokens[pos + 1]] - max) - std::log(sum);
        nll -= logp;
        ++scored;
        if (argmax(lg, im.n_vocab) == tokens[pos + 1]) ++top1;
        return true;
    };

    if (req.step) {
        // The decode regime, token by token (see PplRequest::step). The unscored prefix goes in as
        // one prefill batch — labelled as such, so a decode-only policy stays out of it exactly as
        // it does in generation — and every scored position is its own one-token decode.
        const int prefix = std::max(1, std::min(req.skip, n - 1));
        batch_fill(b, tokens.data(), prefix, /*pos0*/ 0, /*all_logits*/ false);
        im.hook->set_batch_phase(0);
        if (llama_decode(ctx, b) != 0) {
            r.error = "prefill decode failed";
            return r;
        }
        // With choices the last token is fed too, so the final distribution exists.
        const int last = req.choices.empty() ? n - 1 : n;
        for (int pos = prefix; pos < last; ++pos) {
            batch_fill(b, tokens.data() + pos, 1, pos, /*all_logits*/ true);
            im.hook->set_batch_phase(1);
            if (llama_decode(ctx, b) != 0) {
                r.error = "decode failed at position " + std::to_string(pos);
                return r;
            }
            last_row = 0;
            if (pos < req.skip || pos + 1 >= n) continue;
            if (!score(0, pos)) return r;
        }
    } else {
        for (int i = 0; i < n; i += im.cfg.n_batch) {
            const int chunk = std::min(im.cfg.n_batch, n - i);
            batch_fill(b, tokens.data() + i, chunk, /*pos0*/ i, /*all_logits*/ true);
            im.hook->set_batch_phase(req.as_decode ? 1 : 0);
            if (llama_decode(ctx, b) != 0) {
                r.error = "decode failed at position " + std::to_string(i);
                return r;
            }
            // Position p predicts token p+1, so the last token of the whole text is never scored
            // and the last row of a chunk predicts the first token of the next one.
            for (int j = 0; j < chunk; ++j) {
                const int pos = i + j;
                if (pos + 1 >= n || pos < req.skip) continue;
                if (!score(j, pos)) return r;
            }
            last_row = chunk - 1;
        }
    }
    if (!req.choices.empty()) {
        const float * lg = llama_get_logits_ith(ctx, last_row);
        if (!lg) {
            r.error = "no logits after the text";
            return r;
        }
        float max = lg[0];
        for (int v = 1; v < im.n_vocab; ++v)
            if (lg[v] > max) max = lg[v];
        double sum = 0.0;
        for (int v = 0; v < im.n_vocab; ++v)
            sum += std::exp((double) (lg[v] - max));
        const double lse = (double) max + std::log(sum);
        for (const std::string & c : req.choices) {
            llama_token ct[8];
            const int nc = llama_tokenize(im.vocab, c.c_str(), (int) c.size(), ct, 8, false, false);
            if (nc < 1) {
                r.error = "choice '" + c + "' does not tokenize";
                return r;
            }
            r.choice_logp.push_back((double) lg[ct[0]] - lse);
        }
    }
    if (scored == 0 && req.choices.empty()) {
        r.error = "nothing scored — text shorter than skip + 1";
        return r;
    }
    r.nll = scored > 0 ? nll / scored : 0.0;
    r.ppl = std::exp(r.nll);
    r.n_scored = scored;
    r.n_top1 = top1;
    r.experts_routed = im.hook->experts_routed() - routed0;
    r.experts_dropped = im.hook->experts_dropped() - dropped0;
    r.experts_reranked = im.hook->experts_reranked() - reranked0;
    r.experts_substituted = im.hook->experts_substituted() - substituted0;
    r.seconds = secs(t0, clock_t_::now());
    r.ok = true;
    return r;
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

    // The gguf header answers several separate questions below — the arch-prefixed key for a
    // top-k override, the route trace's effective top-k, the run info's top-k/expert count, and
    // the streamer's per-tensor file offsets — and each used to reopen and reparse the file for
    // its own answer. Parse it at most once, lazily: the callers are conditional (a run with no
    // override, no trace and no streaming asks nothing), so an eager read would be work the
    // common path never needs. One parse serves both the model info and the offsets.
    GgufMeta gguf_meta;
    bool gguf_meta_read = false;
    auto meta = [&]() -> const GgufMeta & {
        if (!gguf_meta_read) {
            gguf_meta = read_gguf_meta(cfg.model_path.c_str());
            gguf_meta_read = true;
        }
        return gguf_meta;
    };
    auto gguf = [&]() -> const GgufModelInfo & { return meta().info; };

    // Load with the layout the streamer requires: file-backed mmap, no repack (a repacked
    // q4_K buffer would break the rebind), experts on CPU.
    llama_model_params mparams = llama_model_default_params();
    mparams.load_mode = LLAMA_LOAD_MODE_MMAP;
    mparams.use_extra_bufts = false;
    mparams.n_gpu_layers = 0;
    // The nextn/MTP block is skipped at load unless asked for: llama.cpp marks its tensors
    // TENSOR_SKIP by default, and only --mtp builds a graph over them. n_layer_nextn comes from
    // the gguf metadata either way, so the "this model has no trained head" check below is
    // unaffected by this flag.
    mparams.load_mtp = cfg.spec.is_mtp();

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
    // Excluded from n_layer by llama.cpp, so ask for it separately. Zero on every model without a
    // trained MTP head — which is most ggufs, including quants of MTP-capable models that were
    // converted without the nextn tensors.
    im.n_layer_nextn = llama_model_n_layer_nextn(model);
    if (cfg.spec.is_mtp() && im.n_layer_nextn <= 0)
        return fail("--mtp needs a model with a trained MTP head, and this gguf has no nextn block "
                    "(nextn_predict_layers is absent or zero). Qwen3.5/3.6 carry one in their "
                    "ordinary quantisations; other architectures have none. --ngram speculates on any "
                    "model, since it drafts from the text rather than from the weights. See "
                    "docs/mtp.md and docs/ngram.md.");

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

    // The MTP block lives at layer index n_layer and routes experts of its own, so with speculation
    // on the hook and the streamer must span the trunk PLUS the head. The index space is contiguous
    // and the tensor naming identical, so this bound is the only thing standing between the streamer
    // and the MTP experts — left at n_layer they are silently skipped and stay mmap-resident.
    const int n_layer_streamed = im.n_layer + (cfg.spec.is_mtp() ? im.n_layer_nextn : 0);
    im.hook = std::make_unique<RouterHook>(recipe ? *recipe : MoeRecipe{}, n_layer_streamed);
    im.hook->set_prefetch_layers(cfg.moe.prefetch_layers);
    im.hook->set_drop_policy(cfg.moe.drop_cold_frac, cfg.moe.drop_renorm, cfg.moe.drop_prefill);
    im.hook->set_expert_substitute(cfg.moe.substitute_lambda);
    im.hook->set_predict_log(cfg.moe.predict_log);
    im.hook->set_predict_prefetch(cfg.moe.predict_prefetch, cfg.moe.predict_spec_max);
    im.hook->set_route_ahead(cfg.moe.route_ahead);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = cfg.n_ctx;
    cparams.n_batch = cfg.n_batch;
    // The graph is reserved for the widest ubatch, so this is what sets the resident compute
    // buffers — the memory this engine is always short of. 0 keeps the historical behaviour
    // (one graph as wide as the batch); a smaller value chunks prefill to buy that memory back.
    cparams.n_ubatch = cfg.n_ubatch > 0 ? (uint32_t) cfg.n_ubatch : (uint32_t) cfg.n_batch;
    // The streamer needs the callback to see routing; the compute trace needs it to time nodes.
    // Installing it for the trace alone is what lets a NON-streamed run be measured — the dense
    // mmap baseline the streamed numbers are argued against.
    if (cfg.moe.enabled || compute_trace) {
        cparams.cb_eval = &RouterHook::c_eval;
        cparams.cb_eval_user_data = im.hook.get();
    }
    // Rejecting a draft means rewinding the KV to the last accepted position. With recurrent-state
    // snapshots the rewind is a cheap restore; without them llama.cpp has to fall back to replaying
    // the sequence, which would hand back exactly the decode the speculation just saved.
    if (cfg.spec.enabled()) cparams.n_rs_seq = (uint32_t) cfg.spec.draft_max;

    llama_context * ctx = llama_init_from_model(model, cparams);
    if (!ctx) return fail("failed to create context");
    im.ctx.reset(ctx);
    llama_set_n_threads(ctx, cfg.n_threads, cfg.n_threads);

    // The MTP draft context: same model, same eval callback, but ctx_type = MTP so llama.cpp builds
    // the nextn graph. It keeps its own (single-position) KV, hence n_rs_seq = 0 — nothing is ever
    // rolled back on the draft side, the target is the one that speculates.
    if (cfg.spec.is_mtp()) {
        llama_context_params dparams = cparams;
        dparams.ctx_type = LLAMA_CONTEXT_TYPE_MTP;
        dparams.n_rs_seq = 0;
        // Compute buffers are reserved for the WIDEST ubatch, and left at the target's width the
        // draft context reserves gigabytes for a graph it never runs: the widest thing it ever
        // computes is a prefill chunk, and at decode time it is 1 + draft_max positions. On this
        // engine every MiB reserved is a MiB the expert cache does not get, and the cache is what
        // decides whether the widened verify read set is a hit or a flash read — so an oversized
        // draft context does not merely waste memory, it eats the feature's own prize. Measured on
        // the desktop host: leaving it at the target's width cost ~1 GiB of expert cache and 3
        // points of hit rate. n_batch stays as it is — the speculative driver sizes its internal
        // batch from it and must still accept a whole prefill chunk, which llama.cpp then splits
        // into ubatches of the width below.
        dparams.n_ubatch = (uint32_t) std::max(cfg.spec.draft_max + 1, mtp_draft_ubatch);
        if (dparams.n_ubatch > cparams.n_ubatch) dparams.n_ubatch = cparams.n_ubatch;
        llama_context * ctx_dft = llama_init_from_model(model, dparams);
        if (!ctx_dft) return fail("failed to create the MTP draft context");
        im.ctx_dft.reset(ctx_dft);
        llama_set_n_threads(ctx_dft, cfg.n_threads, cfg.n_threads);
    }

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

    // Built before the capture warm-up on purpose: the driver's constructor turns on nextn
    // extraction for both contexts, which is what the target graph looks like for the rest of the
    // session. Capturing the graph it actually runs beats capturing the one it briefly had.
    if (cfg.spec.is_mtp()) {
        common_params_speculative sp;
        sp.types = {COMMON_SPECULATIVE_TYPE_DRAFT_MTP};
        sp.draft.n_max = cfg.spec.draft_max;
        sp.draft.n_min = 0; // never skip a whole draft; width is bounded by n_max and p_min below
        // The head's own confidence floor for continuing to draft. At 0 (the default) it drafts
        // n_max tokens however unsure it is, which is what the host was measured at; above 0 the
        // width becomes adaptive per step. See SpecConfig::draft_p_min for why that pays twice on a
        // streamed device.
        sp.draft.p_min = cfg.spec.draft_p_min;
        sp.draft.ctx_tgt = ctx; // self-speculation: one model, two contexts over it
        sp.draft.ctx_dft = im.ctx_dft.get();
        im.mtp.reset(common_speculative_init(sp, /*n_seq*/ 1));
        if (!im.mtp) return fail("failed to initialise MTP speculative decoding");
    }

    // The wide verify batch belongs to the loop, not to a source: whoever drafted, the target is
    // handed 1 + draft_max positions in one decode. Allocated for any speculation, which is what
    // lets the n-gram source reuse the whole verify half without a draft context.
    if (cfg.spec.enabled()) {
        // One batch for the session, wide enough for the larger of its two roles.
        im.mtp_batch = llama_batch_init(std::max(cfg.n_batch, cfg.spec.draft_max + 1), /*embd*/ 0, /*n_seq_max*/ 1);
        im.mtp_batch_owned = true;
        im.draft_buf.reserve((size_t) cfg.spec.draft_max);
    }

    if (cfg.moe.enabled) {
        // Capture warm-up: one mmap-resident decode so the eval-callback can harvest the expert
        // tensor pointers from the graph. Any valid token builds the same graph (the expert
        // tensor structure is prompt-independent), so use BOS; KV is wiped afterwards.
        im.hook->begin_capture();
        llama_token warm_tok = llama_vocab_bos(im.vocab);
        if (warm_tok < 0) warm_tok = 0;
        if (cfg.spec.is_mtp()) {
            batch_fill(im.mtp_batch, &warm_tok, 1, /*pos0*/ 0, /*all_logits*/ true);
            if (llama_decode(ctx, im.mtp_batch) != 0) return fail("capture warm-up decode failed");
            // The MTP graph is built only by a decode on the draft context, and process() is what
            // issues it. Without this pass the head's expert layer never reaches the eval callback,
            // so it would stay unbound and silently mmap-resident — the one thing streaming exists
            // to avoid on a model that does not fit.
            if (!common_speculative_process(im.mtp.get(), im.mtp_batch))
                return fail("MTP capture warm-up failed: the draft context could not process the batch");
        } else {
            llama_batch warm = llama_batch_get_one(&warm_tok, 1);
            if (llama_decode(ctx, warm) != 0) return fail("capture warm-up decode failed");
        }
        im.hook->end_capture();

        const GgufOffsets & offs = meta().offsets;
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
                L.proj[p].file_idx = offs.file_by_name.at(t->name); // same parse as the offset, so present
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
        if (route_trace) dense_bytes = dense_bytes_per_layer(offs, layers, n_layer_streamed);

        // Hand the streamer the dense (non-expert) model weights: every captured weight leaf that IS
        // a gguf tensor (dropping graph inputs and KV, which share the leaf shape) and is NOT one of
        // the streamed experts. Built before init consumes `layers`. Anonymous/Pinned read these into
        // their own buffers; every mode needs the list to hold back a tensor too large to be
        // resident at all (qwen4exp's n-gram table), which must also leave the warm sweep and the
        // residency sensor — see DenseWeights::hold_back_oversized.
        {
            const std::unordered_set<std::string> expert_names = expert_tensor_names(layers);
            // The subset the graph only row-gathers, when the run asked for the row policy. Their
            // membership is the hook's verdict on what the graph did, and this is a filter over the
            // dense list rather than a second list, so a table cannot be both resident and streamed.
            const std::unordered_set<std::string> row_names =
                cfg.moe.row_stream ? im.hook->row_gathered_weights() : std::unordered_set<std::string>();
            std::vector<DenseTensorRef> dense, rows;
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
                d.file_idx = offs.file_by_name.at(name);
                dense.push_back(d);
                if (row_names.count(name)) rows.push_back(d);
            }
            const uint64_t row_budget = (uint64_t) std::max(0, cfg.moe.row_stream_mb) * 1024ull * 1024ull;
            im.source.set_dense_tensors(std::move(dense));
            im.source.set_row_tensors(std::move(rows), row_budget);
        }

        if (!im.source.init(offs.shard_paths, n_expert, std::move(layers), cfg.moe))
            return fail("expert stream source init failed");
        im.hook->set_source(&im.source);
        // The row policy exists only once dense_.init has taken the tables over; null means nothing
        // qualified or the takeover declined, and the hook then costs exactly nothing per node.
        im.hook->set_row_source(im.source.row_source());

        if (route_trace) {
            im.route_trace = route_trace;
            im.hook->set_trace(true);

            RouteTraceStatic st;
            st.model = cfg.model_path;
            st.arch = im.arch;
            // The streamed span, not the trunk: with speculation on, the MTP block is a layer the
            // streamer manages like any other, and a trace that stopped at the trunk would drop it.
            st.n_layer = n_layer_streamed;
            st.n_expert = n_expert;
            // The effective top-k: an override IS the applied width, otherwise the model's own.
            st.n_expert_used = cfg.n_expert_used;
            if (st.n_expert_used <= 0) {
                const GgufModelInfo & info = gguf();
                st.n_expert_used = info.ok ? info.n_expert_used : 0;
            }
            st.dense_bytes_per_layer = std::move(dense_bytes);
            st.expert_bytes_per_layer.resize((size_t) n_layer_streamed);
            for (int il = 0; il < n_layer_streamed; ++il)
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
        if (im.ctx_dft) llama_memory_clear(llama_get_memory(im.ctx_dft.get()), true);
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
        st.o_direct = cfg.moe.enabled && im.source.stats().o_direct; // the open's outcome, not the request
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
        ri.engine_version = version();
        ri.n_threads = cfg.n_threads;
        ri.n_ctx = cfg.n_ctx;
        ri.n_ubatch = cfg.n_ubatch;
        ri.chatml = cfg.chatml;
        ri.compute_trace_layers = cfg.compute_trace_layers;
        ri.spec = cfg.spec.is_mtp() ? "mtp" : cfg.spec.is_ngram() ? "ngram" : "off";
        ri.spec_draft_max = cfg.spec.enabled() ? cfg.spec.draft_max : 0;
        ri.mtp_p_min = cfg.spec.is_mtp() ? cfg.spec.draft_p_min : 0.0f;
        ri.ngram_min_match = cfg.spec.is_ngram() ? cfg.spec.ngram_min_match : 0;
        ri.temp = cfg.sampling.temp;
        ri.top_k = cfg.sampling.top_k;
        ri.top_p = cfg.sampling.top_p;
        ri.seed = cfg.sampling.seed;
        ri.moe_stream = cfg.moe.enabled;
        ri.cache_auto = cfg.moe.cache_auto;
        ri.cache_floor_mb = cfg.moe.cache_floor_mb;
        ri.cache_ceil_mb = cfg.moe.cache_ceil_mb;
        ri.force_cache = cfg.moe.force_cache;
        ri.load_all = cfg.moe.enabled && cfg.moe.load_all;
        ri.io_threads = cfg.moe.enabled ? cfg.moe.io_threads : 0;
        ri.overlap = cfg.moe.enabled && cfg.moe.overlap;
        ri.io_two_wave = cfg.moe.enabled && cfg.moe.io_two_wave;
        ri.prefetch_layers = cfg.moe.enabled ? cfg.moe.prefetch_layers : 0;
        ri.route_ahead = cfg.moe.enabled ? cfg.moe.route_ahead : 0;
        ri.predict_prefetch = cfg.moe.enabled && cfg.moe.predict_prefetch;
        ri.predict_log = cfg.moe.enabled && cfg.moe.predict_log;
        ri.predict_spec_max = cfg.moe.enabled ? cfg.moe.predict_spec_max : 0;
        ri.prefetch_sync = cfg.moe.enabled && cfg.moe.prefetch_sync;
        ri.drop_cold_frac = cfg.moe.enabled ? cfg.moe.drop_cold_frac : 0.0f;
        ri.drop_renorm = cfg.moe.drop_renorm;
        ri.drop_prefill = cfg.moe.drop_prefill;
        ri.substitute_lambda = cfg.moe.enabled ? cfg.moe.substitute_lambda : 0.0f;
        // The CSV keeps the two familiar flags, derived from the resolved dense-weights policy.
        ri.dense_weights = cfg.moe.dense_weights == DenseWeightsMode::Mmap        ? "mmap"
                           : cfg.moe.dense_weights == DenseWeightsMode::Anonymous ? "anon"
                           : cfg.moe.dense_weights == DenseWeightsMode::Pinned    ? "ahwb"
                                                                                  : "warm";
        if (cfg.moe.enabled) {
            const IExpertSource::Stats st = im.source.stats();
            ri.cache_mb = (int) (st.cache_budget_bytes / (1024ull * 1024ull));
            // o_direct from the same sample: whether the shard readers actually got cache bypass
            // (O_DIRECT honoured, or F_NOCACHE applied on Apple), never what the flag asked for.
            ri.o_direct = st.o_direct;
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
        // Last, because it needs both numbers above: the budget the streamer settled on and the
        // width that was actually applied. Rounded UP — a cycle reported as smaller than it is
        // would put a budget on the wrong side of the cliff in exactly the borderline case.
        ri.cache_cycle_mb =
            (int) ((im.source.worst_cycle_bytes(ri.n_expert_used) + 1024ull * 1024ull - 1) / (1024ull * 1024ull));
    }

    // The effective routing width, resolved once: an override IS the applied width, otherwise the
    // model's own count. Exposed so a UI can interpret drop_cold_frac, and used by the warning below.
    im.n_expert_used = cfg.n_expert_used;
    if (im.n_expert_used <= 0) {
        const GgufModelInfo & mi = gguf();
        im.n_expert_used = mi.ok ? mi.n_expert_used : 0;
    }

    // Cache-aware dropping on a narrow routing: say so once, at load.
    //
    // The threshold is a fraction of the uniform share 1/top-k, so what it removes scales with how
    // wide the routing is. At top-k 8 (where it was measured) it trims a long tail; at top-k 2 the
    // same fraction can discard the whole minority expert on every miss, which is closer to halving
    // the routing than to trimming it. The engine has no way to know whether that is acceptable for
    // a given model, so it states the fact rather than clamping the flag — a silent adjustment
    // would be worse than a loud caveat.
    if (cfg.moe.enabled && cfg.moe.drop_cold_frac > 0.0f) {
        const int k = im.n_expert_used;
        if (k > 0 && k <= MoeStreamConfig::drop_low_topk_warn)
            std::fprintf(stderr,
                         "bmoe: WARNING drop-cold-experts=%.2f with top-k %d — the threshold is %.1f%% of the "
                         "routing here, against 12.5%% at the top-k 8 this was measured on. Expect it to discard "
                         "much more, and check output quality on your own task.\n",
                         (double) cfg.moe.drop_cold_frac, k, 100.0 * cfg.moe.drop_cold_frac / k);
    }

    // A cache budget below one token's worst-case cycle: say so once, at load.
    //
    // The number is computable from the model's shape alone, so this costs nothing and is available
    // at the only moment it can still be acted on. It is a warning, not a rejection: the budget is
    // legal, the run is byte-correct, and cache_min_mb already rejects the band it was drawn for.
    // What this catches is the case that number cannot see — a budget comfortably above the fixed
    // floor and still under THIS model's cycle, which --n-expert-used widens without touching the
    // budget. Below the cycle no entry survives to the next token, so the run pays management and
    // RAM for a cache that cannot hit. The engine states the fact; what to do about it is
    // docs/cache-sizing.md's business, not a knob the engine should editorialize about.
    if (im.info.cache_mb > 0 && im.info.cache_cycle_mb > 0 && im.info.cache_mb < im.info.cache_cycle_mb) {
        std::fprintf(stderr,
                     "bmoe: WARNING expert cache %d MiB is below this model's worst-case token cycle of %d MiB "
                     "at top-k %d — no entry can survive to the next token, so expect a hit rate near zero.\n",
                     im.info.cache_mb, im.info.cache_cycle_mb, im.info.n_expert_used);
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
        // The draft context tracks the target's positions and must be dropped with it, or the first
        // draft of the new conversation is conditioned on the previous one.
        if (im.ctx_dft) llama_memory_clear(llama_get_memory(im.ctx_dft.get()), true);
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
        // The draft context mirrors the target's positions, so it has to be rewound to the SAME
        // point — including when n_common did not move, since the previous turn left it holding
        // everything it generated. Miss this and the first prefill batch of the turn starts at a
        // position the draft context already has, which llama.cpp rejects outright: the second
        // message of a conversation fails while the first always works.
        if (im.ctx_dft && !llama_memory_seq_rm(llama_get_memory(im.ctx_dft.get()), 0, (llama_pos) n_common, -1))
            llama_memory_clear(llama_get_memory(im.ctx_dft.get()), true);
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
        // Whatever the target rolled back to, the draft context follows: a cancelled turn that left
        // the two at different positions would fail the NEXT turn, not this one.
        if (im.ctx_dft) {
            const llama_pos keep = chat_on ? (llama_pos) n_common : 0;
            if (!llama_memory_seq_rm(llama_get_memory(im.ctx_dft.get()), 0, keep, -1))
                llama_memory_clear(llama_get_memory(im.ctx_dft.get()), true);
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
    // Prefill attribution (#173): the cumulative counters are pinned before the prompt chunks so
    // their deltas across prefill carry the same wall-additive terms the decode phase reports.
    // The session layer already holds everything needed; the streamer is untouched.
    PrefillTally prefill_tally;
    prefill_tally.begin(moe.enabled, im.source);
    const auto t_prefill0 = clock_t_::now();
    // Two predicates, deliberately distinct. spec_on is "the verify loop runs" — a wide batch, an
    // accept pass, a rollback — and both sources need all of it. mtp_on is "the draft comes from the
    // head", which is the only thing that needs the second context and llama.cpp's `common`
    // speculative driver. Conflating them is what would make the n-gram source pay for a draft
    // context it never uses.
    const bool spec_on = im.cfg.spec.enabled();
    const bool mtp_on = im.mtp != nullptr;
    for (int i = (int) n_common; i < n_prompt; i += im.cfg.n_batch) {
        const int chunk = std::min(im.cfg.n_batch, n_prompt - i);
        llama_batch pf;
        if (mtp_on) {
            // Positions are absolute here — the prompt token at index i sits at position i, reused
            // prefix included — which is exactly what llama_batch_get_one would have inferred.
            batch_fill(im.mtp_batch, tokens.data() + i, chunk, /*pos0*/ i, /*all_logits*/ false);
            pf = im.mtp_batch;
        } else {
            pf = llama_batch_get_one(tokens.data() + i, chunk);
        }
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
        // The draft context has to walk the prompt too: its KV must reach the last prompt position
        // or the first draft is made from a hidden state that never saw the prompt.
        if (mtp_on && !common_speculative_process(im.mtp.get(), pf))
            return fail("MTP draft context failed to process the prefill batch");
    }
    // The suffix is now in the KV; record it so the next turn can diff against it.
    if (chat_on)
        for (int i = (int) n_common; i < n_prompt; ++i)
            im.kv_tokens.push_back(tokens[i]);
    // Announced only once the prompt is in both contexts: begin() checks how far the draft context
    // has actually got, so calling it before prefill would warn about a gap that is about to close.
    if (mtp_on) common_speculative_begin(im.mtp.get(), /*seq_id*/ 0, tokens);
    const double prefill_seconds = secs(t_prefill0, clock_t_::now());
    // Prefill attribution, closed here (the begin() snapshot sits above the chunk loop): deltas
    // of the cumulative counters across the prompt, the quantities the decode phase reports per
    // token. Read with the same rules: io is summed lane busy time under overlap, stall is the
    // union of stalled intervals, cpu is whole-process (upper bound on compute-thread time).
    prefill_tally.end(moe.enabled, im.source);
    const float * logits = llama_get_logits_ith(ctx, -1);

    // ── greedy generation ──
    res.ok = true;
    std::string gen;
    int n_gen = 0;
    double gen_seconds = 0.0;

    // Baseline seeded from prefill's closing sample: the summary reports the generation phase
    // only, from real per-token deltas (prefill routes near the whole bank, so folding it into a
    // per-token average would badly inflate the flash-I/O figure). Nothing runs between the two
    // phases, so end()'s snapshot IS the decode baseline — one reading of the counters at the
    // boundary instead of two that could drift apart. In a warm session these counters carry the
    // prior prompts' totals; the deltas make each prompt self-relative.
    GenTally tally;
    tally.overlap = moe.overlap;
    if (moe.enabled) {
        const IExpertSource::Stats & st0 = prefill_tally.post;
        tally.prev_bytes = (long long) st0.read_bytes;
        tally.prev_io_s = st0.read_seconds;
        tally.prev_mgmt_s = st0.mgmt_seconds;
        tally.prev_stall_s = st0.stall_seconds;
        tally.prev_drain_s = st0.drain_wait_seconds;
        tally.prev_adopt_s = st0.adopt_wait_seconds;
    }
    const IExpertSource::Stats st_spec0 = moe.enabled ? im.source.stats() : IExpertSource::Stats{};
    long long prev_spec_bytes = (long long) st_spec0.spec_read_bytes;
    long long prev_spec_experts = st_spec0.spec_experts;
    long long prev_spec_useful = st_spec0.spec_useful;
    // Taken after prefill, so the drop counters describe generation — the phase the policy is armed
    // for and the one the tok/s number is about.
    const long long prev_routed = im.hook->experts_routed();
    const long long prev_dropped = im.hook->experts_dropped();
    const long long prev_reranked = im.hook->experts_reranked();
    const long long prev_substituted = im.hook->experts_substituted();
    // Per-token cursors for the hook's own eval-thread meters (route-ahead issue + watchdog): the
    // hook accumulates for the session, the rows want this token's share.
    long long prev_ra_issue_ns = im.hook->route_ahead_issue_ns();
    long long prev_ra_wd_ns = im.hook->route_ahead_wd_ns();

    // The decode bracket below measures llama_decode and nothing else, which is what makes
    // compute_ms a clean residual — but it also means everything BETWEEN two decodes (sampling,
    // detokenization, rendering, the sinks) is charged to nobody and disappears from tok/s. Mark
    // where the last decode ended so each token can report the gap it actually waited through.
    auto loop_mark = clock_t_::now();
    double loop_overhead_s = 0.0;

    // Absolute position the next decoded token occupies. Prefill left the context filled up to
    // n_prompt-1; without speculation this simply tracks n_prompt + n_gen, but a verify decode
    // advances it by a whole accepted group, so it is carried explicitly.
    llama_pos n_past = n_prompt;
    // The token sequence the draft source conditions on: the prompt plus everything confirmed since.
    // Only built when speculating — the plain path has no use for it. The head reads it as the
    // sequence to seed from; the n-gram source searches it, and it IS the whole corpus.
    std::vector<llama_token> mtp_ctx;
    if (spec_on) mtp_ctx = tokens;
    std::vector<llama_token> verify_toks; // [confirmed token, drafts...] for the verify batch
    std::vector<llama_token> confirmed;   // what one decode confirmed, in order
    const long long mtp0_drafted = im.mtp_drafted;
    const long long mtp0_accepted = im.mtp_accepted;
    const long long mtp0_decodes = im.mtp_decodes;
    const long long mtp0_drafted_steps = im.drafted_steps;
    const double mtp0_draft_s = im.mtp_draft_seconds;
    const uint64_t mtp0_draft_bytes = im.mtp_draft_read_bytes;

    // The token the last decode settled on, not yet in the KV. Greedy stays argmax (byte-identical
    // to the resident reference the gates check); with a sampling chain, draw from the context's
    // last-position logits, which llama_sampler_sample reads at index -1 — the same logits argmax
    // would have read.
    llama_token tok = im.smpl ? llama_sampler_sample(im.smpl, ctx, -1) : argmax(logits, im.n_vocab);

    while (n_gen < req.n_predict) {
        if (llama_vocab_is_eog(im.vocab, tok)) break;

        // ── draft ──
        // The source proposes a continuation of `tok`, capped at the caller's remaining budget: a
        // draft accepted past n_predict would be verified, charged for, and then discarded. The cap
        // goes in BEFORE drafting, so no source is ever asked for tokens with nowhere to go.
        int n_draft = 0;
        double draft_s = 0.0;                       // this group's drafting + catch-up (see below)
        const int room = req.n_predict - n_gen - 1; // tokens still wanted after `tok` itself
        if (spec_on && room > 0) {
            const auto d0 = clock_t_::now();
            const uint64_t db0 = moe.enabled ? im.source.stats().read_bytes : 0;
            im.draft_buf.clear();
            if (mtp_on) {
                common_speculative_draft_params & dp = common_speculative_get_draft_params(im.mtp.get(), /*seq*/ 0);
                dp.drafting = true;
                dp.n_max = std::min(im.cfg.spec.draft_max, room);
                dp.n_past = n_past;
                dp.id_last = tok;
                dp.prompt = &mtp_ctx;
                dp.result = &im.draft_buf;
                common_speculative_draft(im.mtp.get());
                if ((int) im.draft_buf.size() > room) im.draft_buf.resize((size_t) room);
                n_draft = (int) im.draft_buf.size();

                // Drafting WROTE into the draft context's KV at the very positions the catch-up
                // below is about to occupy. Rewind to where the draft started, or the second decode
                // collides with the first (llama.cpp requires a batch to begin strictly after the
                // last stored position). The catch-up is what replaces those rows with ones
                // conditioned on the target's own hidden states instead of the head's guesses.
                if (!llama_memory_seq_rm(llama_get_memory(im.ctx_dft.get()), /*seq*/ 0, n_past, -1))
                    return fail("the MTP draft context does not support rewinding its KV cache");
            } else {
                // The n-gram source reads the text and nothing else: no draft context, no decode, no
                // expert read, so the bytes bracketed around this arm are zero by construction. When
                // it finds no confident match it returns 0 and the step below is an ordinary
                // single-token decode — the property the head does not have, and the reason the
                // floor of this source is the baseline rather than a loss.
                n_draft = ngram_draft(mtp_ctx, tok, std::min(im.cfg.spec.draft_max, room), im.cfg.spec.ngram_min_match,
                                      im.cfg.spec.ngram_max_match, im.draft_buf);
            }
            im.mtp_drafted += n_draft;
            if (n_draft > 0) ++im.drafted_steps;
            draft_s += secs(d0, clock_t_::now());
            if (moe.enabled) im.mtp_draft_read_bytes += im.source.stats().read_bytes - db0;
        }

        // ── verify batch: the confirmed token, then every draft, all asking for logits ──
        //
        // With nothing drafted there is nothing to verify, so the step takes the plain path — one
        // token, one logits row, no rollback. That is not an optimisation of the speculative loop,
        // it IS the loop's floor: a step that drafts nothing must cost exactly what it would have
        // cost with speculation off, or a source that abstains would still be paying for the
        // scaffolding. It applies to the head too, whenever p_min stops it drafting.
        const bool wide = spec_on && n_draft > 0;
        llama_batch step;
        if (spec_on) {
            verify_toks.clear();
            verify_toks.push_back(tok);
            // n_draft, not draft_buf.size(): on the last token of a run there is no room to draft
            // and the buffer still holds the previous step's proposal.
            verify_toks.insert(verify_toks.end(), im.draft_buf.begin(), im.draft_buf.begin() + n_draft);
        }
        if (wide) {
            batch_fill(im.mtp_batch, verify_toks.data(), (int) verify_toks.size(), n_past, /*all_logits*/ true);
            step = im.mtp_batch;
        } else {
            step = llama_batch_get_one(&tok, 1);
        }

        // Bracket ONLY the decode: major faults and CPU-time deltas here decompose this token's
        // compute residual into flash-fault stalls vs. genuine (or throttled) computation.
        const uint64_t f0 = pio::major_faults();
        const double c0 = pio::process_cpu_seconds();
        auto s0 = clock_t_::now();
        const double overhead = secs(loop_mark, s0); // everything since the previous decode returned
        loop_overhead_s += overhead;
        trace_begin(n_past, /*n_tokens*/ 1 + n_draft, /*phase*/ 1);
        int dec = llama_decode(ctx, step);
        auto s1 = clock_t_::now();
        loop_mark = s1; // the next token's overhead is measured from here
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
        ++im.mtp_decodes;

        // ── accept ──
        // `tok` was settled before the decode, so it is confirmed unconditionally. Each draft is
        // confirmed only while it matches what the target itself would have produced at that
        // position — no approximation enters here. The batch's arithmetic is still not bit-identical
        // to a single-token pass, so a near-tie can land differently; see docs/mtp.md.
        confirmed.clear();
        confirmed.push_back(tok);
        int n_acc = 0;
        bool eog_hit = false;
        for (int i = 0; i < n_draft; ++i) {
            const llama_token want = argmax(llama_get_logits_ith(ctx, i), im.n_vocab);
            if (want != im.draft_buf[i]) break;
            ++n_acc; // accepted: it is in the KV whether or not the caller gets to see it
            if (llama_vocab_is_eog(im.vocab, want)) {
                eog_hit = true; // end-of-generation is never emitted, here as on the plain path
                break;
            }
            confirmed.push_back(want);
        }
        if (spec_on) {
            im.mtp_accepted += n_acc;

            // Catch the draft context up on the ACCEPTED PREFIX ONLY.
            //
            // The catch-up re-runs the MTP block over the batch so the draft context holds rows
            // conditioned on the target's own hidden states, and accept() below seeds the next draft
            // from the row at index n_acc. Handing it the whole verify batch — the obvious ordering,
            // and the one upstream's own loop uses — computes the rejected tail as well, and that
            // tail is deleted a few statements later: those drafts were wrong and nothing ever reads
            // their rows. Acceptance depends only on the target's logits, which are already in hand
            // by this point, so the tail simply never has to be submitted.
            //
            // Identical state, strictly less work. accept() seeds from row min(n_acc, n_rows-1),
            // which is row n_acc under either batch, and the KV this leaves behind is exactly the
            // range the rollback used to carve out. What it saves is (n_draft - n_acc) positions
            // through the MTP block — and that block routes experts of its own, so on a streamed
            // device the saving is flash reads, not just arithmetic.
            //
            // The n-gram source has no state to catch up: its next draft is read off the token
            // history the emit block appends to, which is already correct by the time it is read.
            if (mtp_on) {
                const auto p0 = clock_t_::now();
                const uint64_t pb0 = moe.enabled ? im.source.stats().read_bytes : 0;
                batch_fill(im.mtp_batch, verify_toks.data(), 1 + n_acc, n_past, /*all_logits*/ false);
                if (!common_speculative_process(im.mtp.get(), im.mtp_batch))
                    return fail("MTP draft context failed to process the verify batch");
                draft_s += secs(p0, clock_t_::now());
                if (moe.enabled) im.mtp_draft_read_bytes += im.source.stats().read_bytes - pb0;
            }
            im.mtp_draft_seconds += draft_s;

            // Drop the rejected tail from the target; the bounded-rollback snapshots asked for at
            // context creation are what make this a restore rather than a replay. The draft context
            // needs no rollback of its own — it was never given the tail.
            if (n_acc < n_draft) {
                const llama_pos keep = n_past + 1 + n_acc;
                if (!llama_memory_seq_rm(llama_get_memory(ctx), /*seq*/ 0, keep, -1))
                    return fail("failed to roll back the rejected draft tokens from the KV cache");
            }
            if (mtp_on) common_speculative_accept(im.mtp.get(), /*seq*/ 0, (uint16_t) n_acc);
        }

        // ── emit ──
        // One metrics row per confirmed token, but ONE decode produced them all: its cost goes to
        // the first row and the rest carry zeros (see TokenMetrics::mtp_batch). Splitting the wall
        // evenly would read as several equally-cheap tokens, which is not what happened.
        const double wall = secs(s0, s1);
        gen_seconds += wall;
        const IExpertSource::Stats st = moe.enabled ? im.source.stats() : IExpertSource::Stats{};
        // Route-ahead's eval-thread meters accumulate per DECODE, and one decode can confirm a whole
        // group, so they are read once here and charged to the group's first row like every other
        // group cost. Reading them inside the loop would advance the cursors once per token and
        // credit the second and later rows with a delta of zero for the wrong reason.
        double ra_issue_ms = 0.0, ra_wd_ms = 0.0;
        {
            const long long ri = im.hook->route_ahead_issue_ns(), rw = im.hook->route_ahead_wd_ns();
            ra_issue_ms = (ri - prev_ra_issue_ns) / 1e6;
            ra_wd_ms = (rw - prev_ra_wd_ns) / 1e6;
            prev_ra_issue_ns = ri;
            prev_ra_wd_ns = rw;
        }
        for (size_t e = 0; e < confirmed.size() && n_gen < req.n_predict; ++e) {
            const llama_token out = confirmed[e];
            char piece[256];
            int np = llama_token_to_piece(im.vocab, out, piece, sizeof(piece), 0, true);
            std::string delta = np > 0 ? std::string(piece, np) : std::string();
            gen += delta;
            if (chat_on) im.kv_tokens.push_back(out); // this token is now in the KV
            if (spec_on) mtp_ctx.push_back(out);
            ++n_gen;

            TokenMetrics m;
            m.step = n_gen;
            m.steps = req.n_predict;
            m.mtp_batch = (int) confirmed.size();
            m.loop_overhead_ms = e == 0 ? overhead * 1000.0 : 0.0;
            // Charged to the group's first row like every other group cost. This is a SLICE of
            // loop_overhead_ms, not an addition to it: both measure time outside the decode.
            m.mtp_draft_ms = e == 0 ? draft_s * 1000.0 : 0.0;
            m.ra_issue_ms = e == 0 ? ra_issue_ms : 0.0;
            m.ra_wd_ms = e == 0 ? ra_wd_ms : 0.0;
            m.piece = delta;
            // Only when someone will read it: the parser cannot resume, so this re-parses everything
            // generated so far on every token, and off the chat path it is a full copy of the same.
            if (req.render_text) {
                ShownView sv = shown_view(gen, /*partial*/ true);
                m.text = std::move(sv.content);
                m.reasoning = std::move(sv.reasoning);
            }
            if (e == 0)
                tally.record(m, wall, f1 - f0, c1 - c0, im.turn, moe.enabled ? &st : nullptr);
            else
                tally.record(m, 0.0, 0, 0.0, im.turn, moe.enabled ? &st : nullptr);
            if (on_token) on_token(m);
            if (sink) sink->on_token(m);
        }

        if (eog_hit) break;
        // The accepted group is now KV-resident, and the logits at the first unverified position
        // hold the target's own continuation — the next token, arrived at for free. Off the
        // speculative path the row index stays -1, exactly as before: one token, one row.
        n_past += 1 + n_acc;
        const int32_t row = wide ? n_acc : -1;
        logits = llama_get_logits_ith(ctx, row);
        tok = im.smpl ? llama_sampler_sample(im.smpl, ctx, row) : argmax(logits, im.n_vocab);
    }

    // Speculation can leave the KV ahead of what the caller received: an accepted end-of-generation
    // token is decoded but never emitted, and a group can be cut short by n_predict. The KV and
    // kv_tokens must agree exactly or the next turn's prefix reuse decodes from a state that never
    // produced this answer, so trim back to what was actually emitted.
    if (spec_on) {
        const llama_pos emitted_end = (llama_pos) n_prompt + n_gen;
        if (!llama_memory_seq_rm(llama_get_memory(ctx), 0, emitted_end, -1)) {
            // Nothing survives that we can still describe, so say so rather than leave kv_tokens
            // asserting a prefix the context no longer holds. The next turn re-prefills in full.
            llama_memory_clear(llama_get_memory(ctx), true);
            im.kv_tokens.clear();
        }
        // The draft context mirrors the target's positions (process() decodes the same batches into
        // it), so it is trimmed to the same point — not cleared, or a continued chat turn would
        // feed it only the new suffix and draft from a state that never saw the conversation.
        if (mtp_on && !llama_memory_seq_rm(llama_get_memory(im.ctx_dft.get()), 0, emitted_end, -1))
            llama_memory_clear(llama_get_memory(im.ctx_dft.get()), true);
    }

    ++im.turn; // this turn is written; label the next one apart

    // ── summary ──
    RunSummary & s = res.summary;
    s.n_generated = n_gen;
    s.gen_seconds = gen_seconds;
    s.s_per_token = n_gen ? gen_seconds / n_gen : 0.0;
    s.tokens_per_second = gen_seconds > 0 ? n_gen / gen_seconds : 0.0;
    // Close the accounting: the tail after the last decode belongs to no row, so add it here.
    loop_overhead_s += secs(loop_mark, clock_t_::now());
    s.loop_overhead_s_per_token = n_gen ? loop_overhead_s / n_gen : 0.0;
    s.n_prompt = n_prompt - (int) n_common; // tokens actually prefilled this turn (after KV reuse)
    s.n_past = chat_on ? (int) im.kv_tokens.size() : n_prompt + n_gen; // total context length now
    s.load_seconds = im.load_seconds;
    s.prefill_seconds = prefill_seconds;
    s.prefill_cpu_seconds = prefill_tally.cpu_seconds;
    s.prefill_read_mib = prefill_tally.read_mib;
    s.prefill_io_seconds = prefill_tally.io_seconds;
    s.prefill_stall_seconds = prefill_tally.stall_seconds;
    s.prefill_mgmt_seconds = prefill_tally.mgmt_seconds;
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
        s.cache_evictions = st.evictions;
        s.cache_rereads = st.rereads;
        const RowSourceStats rs = im.source.row_stats();
        s.row_table_mib = rs.table_bytes / (1024.0 * 1024.0);
        s.row_resident_mib = rs.resident_bytes / (1024.0 * 1024.0);
        s.row_read_mib = rs.bytes_read / (1024.0 * 1024.0);
        s.row_rows = (long long) rs.rows;
        s.row_slab_reads = (long long) rs.slab_reads;
        s.row_evictions = (long long) rs.evictions;
        s.row_io_errors = (long long) rs.io_errors;
        s.moe_drain_s_per_token = n_gen ? tally.drain_seconds / n_gen : 0.0;
        s.moe_adopt_s_per_token = n_gen ? tally.adopt_seconds / n_gen : 0.0;
        s.token_demand_mib = st.token_demand_bytes / (1024.0 * 1024.0);
        s.layer_demand_mib = st.layer_demand_bytes / (1024.0 * 1024.0);
        s.moe_spec_read_mib = ((long long) st.spec_read_bytes - prev_spec_bytes) / (1024.0 * 1024.0);
        s.moe_spec_experts = st.spec_experts - prev_spec_experts;
        s.moe_spec_useful = st.spec_useful - prev_spec_useful;
    }
    s.experts_routed = im.hook->experts_routed() - prev_routed;
    s.experts_dropped = im.hook->experts_dropped() - prev_dropped;
    s.experts_reranked = im.hook->experts_reranked() - prev_reranked;
    s.experts_substituted = im.hook->experts_substituted() - prev_substituted;
    // Per-turn deltas, like every other generation figure here: a warm session's counters are
    // cumulative, and an acceptance rate averaged over earlier prompts would describe none of them.
    s.mtp_drafted = im.mtp_drafted - mtp0_drafted;
    s.mtp_accepted = im.mtp_accepted - mtp0_accepted;
    s.mtp_decodes = im.mtp_decodes - mtp0_decodes;
    s.drafted_steps = im.drafted_steps - mtp0_drafted_steps;
    s.mtp_draft_s_per_token = n_gen > 0 ? (im.mtp_draft_seconds - mtp0_draft_s) / n_gen : 0.0;
    s.mtp_draft_read_mib = (double) (im.mtp_draft_read_bytes - mtp0_draft_bytes) / (1024.0 * 1024.0);
    if (moe.predict_log) {
        // Session totals, not a per-generation delta: these are an accuracy estimate, and every
        // turn's routings are equally valid samples of it. See RunSummary.
        s.predict_stale = im.hook->predict_stale();
        s.predict_stale2 = im.hook->predict_stale2();
        s.predict_prev = im.hook->predict_prev();
        s.predict_self = im.hook->predict_self();
        s.predict_stale_by_layer = im.hook->predict_stale_by_layer();
        s.predict_prev_by_layer = im.hook->predict_prev_by_layer();
        s.predict_self_by_layer = im.hook->predict_self_by_layer();
        s.predict_unscored = im.hook->predict_unscored();
    }
    if (moe.route_ahead > 0) {
        // Session totals, like the probe's: every overridden routing is an equally valid sample of
        // the perturbation the flag buys, whichever turn produced it.
        s.route_ahead_overridden = im.hook->route_ahead_overridden();
        s.route_ahead_passthrough = im.hook->route_ahead_passthrough();
        s.route_ahead_slots = im.hook->route_ahead_slots();
        s.route_ahead_hits = im.hook->route_ahead_hits();
        s.route_ahead_gemv_ns = im.hook->route_ahead_gemv_ns();
        s.route_ahead_gemv_jobs = im.hook->route_ahead_gemv_jobs();
        s.route_ahead_issue_ns = im.hook->route_ahead_issue_ns();
        s.route_ahead_wd_ns = im.hook->route_ahead_wd_ns();
    }
    if (sink) sink->on_summary(s);

    // One non-partial parse of the finished generation, shared by the returned text and the history
    // commit below. They asked the same question of the same string and each paid a full parse for
    // it; a reasoning model's answer is the whole turn's output, so that was not a small double.
    common_chat_msg final_msg;
    bool final_parsed = false;
    if (chat_on && !prefilled_answer) {
        try {
            final_msg = common_chat_parse(gen, /*is_partial*/ false, parse_params);
            final_parsed = true;
        } catch (const std::exception & e) {
            detail::warn_parse_failed_once(e.what());
        }
    }
    if (final_parsed) {
        res.generated_text = final_msg.content;
        res.reasoning_text = final_msg.reasoning_content;
    } else {
        // Not chat, a prefilled turn (the stream IS the answer), or a parse that threw: the raw
        // generation stands on its own, exactly as shown_view would have reported it.
        res.generated_text = gen;
    }

    if (res.cancelled) {
        // Undo the whole turn (KV, fed tokens, and the pushed user message) so the conversation
        // is left exactly as it was before this prompt and stays continuable.
        rollback_turn();
    } else if (chat_on) {
        // Commit the assistant turn to the running conversation. Parsing separates a thinking
        // model's reasoning from the answer; the next turn re-renders history from these messages.
        // Reuses the parse above. A prefilled turn has no turn header in the stream to parse — the
        // generation is the answer verbatim — and is committed without the prefill, so history holds
        // a normal assistant message and the next turn re-renders cleanly whatever this turn's think
        // setting was. A parse that threw falls back the same way.
        common_chat_msg assistant;
        if (final_parsed)
            assistant = std::move(final_msg);
        else
            assistant.content = gen;
        assistant.role = "assistant";
        im.chat_history.push_back(assistant);
    }
    return res;
}

} // namespace bmoe
