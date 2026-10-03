// bmoe-rsbench — reproduce and measure the two hybrid-recurrent-state blockers (ADR-004)
// against the llama.cpp this engine links, using the engine's exact context shape
// (core/src/engine/session.cpp: n_ubatch = n_batch, use_extra_bufts=false).
//
//   bmoe-rsbench reserve <model.gguf>      : create a context with n_rs_seq=64 and decode.
//                                            Reproduces the LFM2/LFM2MOE graph-reserve crash
//                                            (GGML_ASSERT(obj_new), needed 836640 vs available
//                                            836272 — one ggml_tensor object short). Fixed
//                                            upstream by adding the LFM2 archs to the
//                                            linear-attention node budget (ggml-org PR #29085).
//   bmoe-rsbench diverge  <model.gguf> [N] : one rollback cell (single-mode rm, d=3, m=0)
//                                            compared against an identically-fed reference
//                                            context. Exit 0 = EXACT, 5 = DIFFER.
//   bmoe-rsbench sweep    <model.gguf>     : run the depth x post-batch-steps x batch-shape
//                                            matrix on one model. Each cell runs TWO contexts
//                                            with identical token feeds (A: rollback+replay,
//                                            B: plain continuation) and byte-compares their
//                                            greedy streams. Cells print predicted vs observed;
//                                            exit 0 iff every cell matches its prediction.
//
// Snapshot mechanics (pin @ 0e8c83e51, src/llama-memory-recurrent.cpp, src/models/delta-net-base.cpp,
// ggml/src/ggml-cpu/ops.cpp) that the predictions encode:
//   - a ubatch of n_seq_tokens tokens writes only min(n_seq_tokens, K) snapshot planes
//     (K = n_rs_seq + 1); single-token steps therefore refresh ONLY plane 0, leaving
//     planes 1..K-1 at their values from the last multi-token ubatch (GDN staleness);
//   - the conv path's in-ubatch sweep maps slot t to the state (n_seq_tokens - (ch-1) ... )
//     clamped at 0, so a 1-token step collapses ALL K slots to the same "1 back" state
//     (conv too-new);
//   - seq_rm rollback depth d reads plane d ("d back") via the per-seq rs_idx gather, so it
//     is exact only while the needed planes still hold their true "d back" values.
// Resolved mechanism (2026-09-18, qwen35 + lfm2moe; see docs/adr/004 addendum 3):
//   - a ubatch of n tokens writes planes 0..min(n,K)-1 with plane p = state p tokens before
//     its end; single-token steps rewrite only plane 0. A rollback of depth d reads plane d.
//   - NONE of the `sweep` cells below can be restored by any implementation: the wanted state
//     sat in plane 0 (single rm phase — overwritten by the first single step) or in a plane
//     the d-token rm ubatch never wrote (batched — the "d=8 anomaly" was a read of a
//     never-written plane). Their EXACT verdicts were argmax robustness on an insensitive
//     prompt; against the index-shift fix they report REFUSED/INFRA, which is the honest
//     answer. predict() documents vanilla's behavior only.
//   - bitwise comparisons against a differently-shaped reference are meaningless on this
//     backend: with NO rollback, splitting a prefill 10 vs 6+4 moves logits by up to ~3.6
//     (MoE routing flips). Every `statecmp`/`dsteps` DIVERGE has that confound; only the d=0
//     control (identical shapes) is clean. Use `cutsweep` (argmax + shape-control rows).
//
// Public API only (llama.h). Build via the engine's normal configure:
//   cmake -B build -DBMOE_BUILD_TOOLS=ON && cmake --build build --target bmoe-rsbench
#include "llama.h"
#include <algorithm>
#include <string_view>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kNRsSeq   = 32;  // >= max d_rm swept; plane count is K = kNRsSeq + 1
constexpr uint32_t kNCtx     = 256; // prompt + m + d_rm + n_gen must fit
constexpr int      kMaxDRm   = 24;
constexpr int      kMaxM     = 16;

struct Cell {
    const char * mode;   // "single" | "batched"
    int d_rm;            // rollback depth (tokens cut)
    int m;               // single-token decode steps between last multi-token ubatch and seq_rm
};

llama_model * load_model(const char * path, const llama_vocab * & vocab) {
    llama_model_params mp = llama_model_default_params();
    mp.use_extra_bufts = false;  // engine load shape (session.cpp): no repack/extra buffers
    llama_model * model = llama_model_load_from_file(path, mp);
    if (!model) { fprintf(stderr, "bmoe-rsbench: model load failed\n"); exit(1); }
    vocab = llama_model_get_vocab(model);
    return model;
}

llama_context * make_ctx(llama_model * model, uint32_t n_ctx, uint32_t n_rs_seq) {
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx     = n_ctx;
    cp.n_batch   = n_ctx;
    cp.n_ubatch  = n_ctx;  // engine default: n_ubatch = n_batch
    cp.n_seq_max = 1;
    cp.n_rs_seq  = n_rs_seq;
    cp.n_threads = cp.n_threads_batch = 4;
    return llama_init_from_model(model, cp);
}

std::vector<llama_token> tokenize(const llama_vocab * vocab, const char * text) {
    int n = -llama_tokenize(vocab, text, (int)strlen(text), nullptr, 0, true, true);
    std::vector<llama_token> toks(n);
    llama_tokenize(vocab, text, (int)strlen(text), toks.data(), (int)toks.size(), true, true);
    return toks;
}

llama_token sample_greedy(llama_context * ctx) {
    const float * logits = llama_get_logits_ith(ctx, -1);
    const llama_vocab * vocab = llama_model_get_vocab(llama_get_model(ctx));
    int n_vocab = llama_vocab_n_tokens(vocab);
    int best = 0;
    for (int i = 1; i < n_vocab; i++) {
        if (logits[i] > logits[best]) {
            best = i;
        }
    }
    return (llama_token) best;
}

// Decode one token batch (positions auto-assigned from the cell's pos).
bool decode(llama_context * ctx, const llama_token * toks, int n) {
    llama_batch batch = llama_batch_get_one(const_cast<llama_token *>(toks), n);
    return llama_decode(ctx, batch) == 0;
}

std::string detok(const llama_vocab * vocab, const std::vector<llama_token> & toks) {
    std::string s(1024, '\0');
    int n = llama_detokenize(vocab, toks.data(), (int)toks.size(), s.data(), (int)s.size(), 0, true);
    if (n < 0) { s.resize((size_t)(-n) + 1); n = llama_detokenize(vocab, toks.data(), (int)toks.size(), s.data(), (int)s.size(), 0, true); }
    s.resize(n > 0 ? (size_t)n : 0);
    return s;
}

// Greedy continuation from the context's current state. Does NOT touch memory —
// the caller owns all seq_rm/clear decisions (the old diverge mode wiped its pending
// rollback here; that bug invalidated the first exactness baselines).
std::vector<llama_token> gen_greedy(llama_context * ctx, int n_gen, const llama_vocab * vocab) {
    std::vector<llama_token> out;
    llama_token tok = sample_greedy(ctx);
    out.push_back(tok);
    while ((int)out.size() < n_gen && tok != llama_vocab_eos(vocab)) {
        if (!decode(ctx, &tok, 1)) { fprintf(stderr, "bmoe-rsbench: gen decode failed\n"); exit(2); }
        tok = sample_greedy(ctx);
        out.push_back(tok);
    }
    return out;
}

// First index where the streams differ, or -1 if equal (streams may end early on EOS).
int first_diff(const std::vector<llama_token> & a, const std::vector<llama_token> & b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return (int) i;
        }
    }
    return a.size() == b.size() ? -1 : (int) n;
}

const char * predict(const Cell & c) {
    const bool single = strcmp(c.mode, "single") == 0;
    if (single) {
        if (c.m == 0) {
            return c.d_rm == 1 ? "EXACT" : "DIFFER";  // d>=2: conv collapse
        }
        return "DIFFER";  // GDN plane stale by m; conv too-new
    }
    // batched
    return c.m == 0 ? "EXACT" : "DIFFER";  // m>=1: boundary plane d_rm stale by m
}

// One cell = two contexts with identical token feeds; A rolls back and replays,
// B just continues. Byte-compare the n_gen greedy tokens after the feed point.
// Returns: 0 = ran, observed EXACT; 1 = ran, observed DIFFER; 2 = infrastructure
// failure (seq_rm refused / decode failed / reserve sanity broke).
int run_cell(llama_model * model, const llama_vocab * vocab, const std::vector<llama_token> & prompt,
             const std::vector<llama_token> & seed, const Cell & c, bool verbose,
             std::vector<llama_token> & stream_a, std::vector<llama_token> & stream_b) {
    const int P    = (int) prompt.size();
    const int n_rm = c.mode[0] == 's' ? c.d_rm : 1;  // batched rm phase = one d_rm-token decode

    llama_context * ctx_a = make_ctx(model, kNCtx, kNRsSeq);
    llama_context * ctx_b = make_ctx(model, kNCtx, kNRsSeq);
    if (!ctx_a || !ctx_b) {
        printf("CELL %s d=%d m=%d : RESERVE_FAIL (context creation returned null)\n", c.mode, c.d_rm, c.m);
        if (ctx_a) { llama_free(ctx_a); }
        if (ctx_b) { llama_free(ctx_b); }
        return 2;
    }

    // Sanity: both contexts must have snapshots engaged (n_rs_seq survives the allowlist clamp).
    if (llama_n_rs_seq(ctx_a) != kNRsSeq) {
        printf("CELL %s d=%d m=%d : SKIP (n_rs_seq clamped to %u — arch not on rollback allowlist)\n",
               c.mode, c.d_rm, c.m, llama_n_rs_seq(ctx_a));
        llama_free(ctx_a);
        llama_free(ctx_b);
        return 2;
    }

    int status = 2;
    do {
        // Phase 1+2 (both): prefill prompt, then m single-token steps on the seed chain.
        llama_memory_t mem_a = llama_get_memory(ctx_a);
        for (int pass = 0; pass < 2; ++pass) {
            llama_context * ctx = pass == 0 ? ctx_a : ctx_b;
            if (!decode(ctx, prompt.data(), P)) { fprintf(stderr, "bmoe-rsbench: prefill failed\n"); goto done; }
            for (int i = 0; i < c.m; ++i) {
                if (!decode(ctx, &seed[i], 1)) { fprintf(stderr, "bmoe-rsbench: seed step failed\n"); goto done; }
            }
        }

        // Phase 3 (both): the d_rm tokens that A will roll back. single: one decode per token
        // (refreshes only plane 0); batched: one d_rm-token ubatch (refreshes planes 0..d_rm-1).
        for (int pass = 0; pass < 2; ++pass) {
            llama_context * ctx = pass == 0 ? ctx_a : ctx_b;
            for (int i = 0; i < n_rm; ++i) {
                const int n_tok = c.mode[0] == 's' ? 1 : c.d_rm;
                if (!decode(ctx, seed.data() + c.m + i * n_tok, n_tok)) {
                    fprintf(stderr, "bmoe-rsbench: rm-phase decode failed\n");
                    goto done;
                }
                if (c.mode[0] != 's') {
                    break;  // batched: the whole rm phase is one decode
                }
            }
        }

        // Phase 4 (A only): the rollback — single-use, must succeed.
        if (!llama_memory_seq_rm(mem_a, 0, P + c.m, -1)) {
            printf("CELL %s d=%d m=%d : RM_FAIL (seq_rm refused — snapshots not engaged)\n", c.mode, c.d_rm, c.m);
            goto done;
        }

        // Phase 5 (A): replay the cut tail as one batch, then greedy n_gen.
        {
            llama_batch batch = llama_batch_get_one(const_cast<llama_token *>(seed.data() + c.m), c.d_rm);
            if (llama_decode(ctx_a, batch)) { fprintf(stderr, "bmoe-rsbench: replay decode failed\n"); goto done; }
        }
        stream_a = gen_greedy(ctx_a, 8, vocab);

        // Phase 5' (B): continue from the same feed point with no rollback.
        stream_b = gen_greedy(ctx_b, (int) stream_a.size(), vocab);

        status = first_diff(stream_a, stream_b) < 0 ? 0 : 1;
    } while (false);

done:
    llama_free(ctx_a);
    llama_free(ctx_b);
    return status;
}

// Sensitivity probe: does the greedy stream notice a state difference at all on this
// prompt? S1 = prefill, 8 single steps, FULL-CLEAR memory, re-prefill the same 8 tokens
// (zeroed recurrent state), gen. S2 = identical feed without the clear. If the two streams
// match, the prompt's argmax is state-insensitive and EXACT matrix cells are uninformative.
void sensitivity_probe(llama_model * model, const llama_vocab * vocab,
                       const std::vector<llama_token> & prompt, const std::vector<llama_token> & seed) {
    llama_context * s1 = make_ctx(model, kNCtx, 0);
    llama_context * s2 = make_ctx(model, kNCtx, 0);
    if (!s1 || !s2) { printf("SENSITIVITY: context creation failed\n"); return; }
    for (int pass = 0; pass < 2; ++pass) {
        llama_context * ctx = pass == 0 ? s1 : s2;
        decode(ctx, prompt.data(), (int)prompt.size());
        for (int i = 0; i < 8; ++i) { decode(ctx, &seed[i], 1); }
    }
    llama_memory_seq_rm(llama_get_memory(s1), 0, -1, -1);  // full clear (s1 only)
    decode(s1, seed.data(), 8);                            // re-prefill on zeroed state
    const std::vector<llama_token> g1 = gen_greedy(s1, 8, vocab);
    const std::vector<llama_token> g2 = gen_greedy(s2, (int)g1.size(), vocab);
    const bool same = first_diff(g1, g2) < 0;
    printf("SENSITIVITY: %s\n", same ?
           "INSENSITIVE — full-clear vs continuation streams match; EXACT cells are uninformative on this prompt"
         : "SENSITIVE — state differences change the stream; EXACT cells are meaningful");
    fflush(stdout);
    llama_free(s1);
    llama_free(s2);
}

void run_sweep(llama_model * model, const llama_vocab * vocab, bool fox_mode) {
    // Number-chain prompt: short prefill, and the greedy tail keeps counting (never EOS),
    // so the golden seed chain always covers m + d_rm + n_gen tokens. fox mode uses the
    // original (state-sensitive) prompt at ~4x the prefill cost, 4 decisive cells only.
    const char * prompt_text = fox_mode ?
        "The quick brown fox jumps over the lazy dog. Repeat after me, exactly and only: hello world\n" :
        "Count from 1 to 100: 1 2 3 4 5 6 7 8 9";
    const std::vector<llama_token> prompt = tokenize(vocab, prompt_text);
    const int P = (int) prompt.size();

    // Golden chain: feeds every cell (m seed tokens + d_rm rolled-back tokens + gen).
    // Computed once on a snapshot-free context so cell comparisons never depend on it.
    llama_context * gold = make_ctx(model, kNCtx, 0);
    if (!gold) { printf("SWEEP: context creation failed\n"); return; }
    if (!decode(gold, prompt.data(), P)) { printf("SWEEP: prefill failed\n"); llama_free(gold); return; }
    std::vector<llama_token> seed = gen_greedy(gold, kMaxM + kMaxDRm + 8, vocab);
    llama_free(gold);
    if ((int) seed.size() < kMaxM + kMaxDRm + 4) {
        printf("SWEEP: golden chain too short (%zu tokens, EOS early) — extend the prompt\n", seed.size());
        return;
    }

    sensitivity_probe(model, vocab, prompt, seed);

    static const Cell fox_cells[] = {
        {"batched", 3, 0}, {"batched", 8, 4}, {"single", 3, 0}, {"single", 8, 4}
    };
    static const Cell std_cells[] = {
        {"batched",  1, 0}, {"batched",  3, 0}, {"batched",  8, 0}, {"batched", 24, 0},
        {"batched",  1, 4}, {"batched",  3, 4}, {"batched",  8, 4}, {"batched", 24, 4},
        {"single",   1, 0}, {"single",   3, 0}, {"single",   8, 0}, {"single",  24, 0},
        {"single",   1, 4}, {"single",   3, 4}, {"single",   8, 4}, {"single",  24, 4}
    };
    const Cell * cells     = fox_mode ? fox_cells : std_cells;
    const int    n_cells   = fox_mode ? (int)(sizeof(fox_cells) / sizeof(fox_cells[0]))
                                     : (int)(sizeof(std_cells) / sizeof(std_cells[0]));

    char arch_buf[64] = {0};
    llama_model_meta_val_str(model, "general.architecture", arch_buf, sizeof(arch_buf));
    printf("SWEEP %s | prompt=%d tok, n_rs_seq=%u, n_gen=8, axes: mode x d_rm x m\n",
           arch_buf[0] ? arch_buf : "unknown-arch", P, kNRsSeq);
    fflush(stdout);

    int n_pred_ok = 0, n_pred_bad = 0, n_infra = 0;
    for (int ci = 0; ci < n_cells; ++ci) {
        const Cell & c = cells[ci];
        std::vector<llama_token> sa, sb;
        const int rc = run_cell(model, vocab, prompt, seed, c, false, sa, sb);
        const char * pred = predict(c);
        if (rc == 2) {
            n_infra++;
            printf("CELL %d/%d %s d=%d m=%d : INFRA-FAIL\n", ci + 1, n_cells, c.mode, c.d_rm, c.m);
            fflush(stdout);
            continue;
        }
        const bool exact = rc == 0;
        const bool ok    = (strcmp(pred, "EXACT") == 0) == exact;
        ok ? n_pred_ok++ : n_pred_bad++;

        std::string ta, tb;
        const int fd = first_diff(sa, sb);
        if (!exact && fd >= 0) {
            ta = detok(vocab, {sa[fd]});
            tb = detok(vocab, {sb[fd]});
        }
        printf("CELL %d/%d %s d=%2d m=%2d : %s (pred %s)%s%s%s\n", ci + 1, n_cells, c.mode, c.d_rm, c.m,
               exact ? "EXACT" : "DIFFER", pred, ok ? "" : "  << PREDICTION FALSIFIED",
               exact ? "" : "  first diff tok: A=\"", exact ? "" : (ta + "\" B=\"" + tb + "\"").c_str());
        fflush(stdout);
    }
    printf("SWEEP SUMMARY: %d/%d cells matched predictions (%d infra failures)\n",
           n_pred_ok, n_pred_ok + n_pred_bad, n_infra);
    if (n_pred_bad == 0 && n_infra == 0) {
        printf("HYPOTHESIS: SUPPORTED — snapshot-plane staleness + conv 1-slot collapse explain every cell\n");
    } else if (n_pred_bad > 0) {
        printf("HYPOTHESIS: FALSIFIED in %d cell(s) — refine the mechanism\n", n_pred_bad);
    }
}

// ---- cutsweep: the rescuable shape ------------------------------------------
// None of the sweep's cells can be restored exactly by ANY implementation: the
// wanted state either sat in plane 0 (single-mode rm phase, overwritten by the
// first single step) or in a plane the rm ubatch never wrote (batched rm phase of
// exactly d tokens). The shape the index-shift fix rescues is the edit-turn shape:
// prefill P (one ubatch, planes p = S(P-1-p)), m single-token decodes (plane 0
// only), then a rollback CUTTING INTO THE PREFILL by c tokens (depth c + m).
// Vanilla reads plane c+m (stale by m tokens); the fix reads plane c (exact).
// At m = 0 both coincide, so the flip is visible only at m >= 1.
//
// Verdicts are argmax-level (greedy stream vs an identically-fed no-rollback
// context), so each cell carries a SHAPE control row: the same feed with no
// rollback, split as [0,P-c) + [P-c,P+m) vs P + m singles. The backend is
// ubatch-shape dependent (MoE routing flips), so a DIFFER on the rollback row is
// only attributable to the restore when its SHAPE row is SAME.
// n_rs_seq = 8 (K = 9 planes, covers c <= 8): LFM2 archs abort at graph reserve with larger
// values until the node-budget fix (ggml-org/llama.cpp#29085) lands — a separate PR from the
// index-shift fix, so the runner must not depend on it.
constexpr uint32_t kCutRsSeq = 8;

int run_cut_cell(llama_model * model, const llama_vocab * vocab, const std::vector<llama_token> & prompt,
                 const std::vector<llama_token> & seed, int c, int m, bool control,
                 std::vector<llama_token> & sa, std::vector<llama_token> & sb) {
    const int P = (int) prompt.size();
    llama_context * ctx_a = make_ctx(model, kNCtx, kCutRsSeq);
    llama_context * ctx_b = make_ctx(model, kNCtx, kCutRsSeq);
    if (!ctx_a || !ctx_b) {
        fprintf(stderr, "bmoe-rsbench: cut cell c=%d m=%d: context creation failed\n", c, m);
        if (ctx_a) { llama_free(ctx_a); }
        if (ctx_b) { llama_free(ctx_b); }
        return 2;
    }

    // the tail every variant ends up having processed: prompt[P-c..P) + seed[0..m)
    std::vector<llama_token> tail(prompt.begin() + (P - c), prompt.end());
    tail.insert(tail.end(), seed.begin(), seed.begin() + m);

    int status = 2;
    do {
        // B: prefill P as one ubatch, m singles, no rollback
        if (!decode(ctx_b, prompt.data(), P)) { break; }
        bool ok = true;
        for (int i = 0; i < m && ok; ++i) { ok = decode(ctx_b, &seed[i], 1); }
        if (!ok) { break; }

        if (control) {
            // A (shape control): no rollback; prefill [0,P-c) then the tail as ONE ubatch —
            // exactly the ubatch history a perfect restore + replay produces
            if (!decode(ctx_a, prompt.data(), P - c)) { break; }
            if (!decode(ctx_a, tail.data(), (int) tail.size())) { break; }
        } else {
            // A: same feed as B, then rollback into the prefill and replay the tail
            if (!decode(ctx_a, prompt.data(), P)) { break; }
            for (int i = 0; i < m && ok; ++i) { ok = decode(ctx_a, &seed[i], 1); }
            if (!ok) { break; }
            if (!llama_memory_seq_rm(llama_get_memory(ctx_a), 0, P - c, -1)) { status = 3; break; }
            if (!decode(ctx_a, tail.data(), (int) tail.size())) { break; }
        }
        sa = gen_greedy(ctx_a, 8, vocab);
        sb = gen_greedy(ctx_b, (int) sa.size(), vocab);
        status = first_diff(sa, sb) < 0 ? 0 : 1;
    } while (false);
    if (status == 2) { fprintf(stderr, "bmoe-rsbench: cut cell c=%d m=%d: decode failed\n", c, m); }

    llama_free(ctx_a);
    llama_free(ctx_b);
    return status;  // 0 EXACT, 1 DIFFER, 2 infra, 3 refused
}

void run_cut_sweep(llama_model * model, const llama_vocab * vocab) {
    const char * prompt_text =
        "The quick brown fox jumps over the lazy dog. Repeat after me, exactly and only: hello world\n";
    const std::vector<llama_token> prompt = tokenize(vocab, prompt_text);
    const int P = (int) prompt.size();

    llama_context * gold = make_ctx(model, kNCtx, 0);
    if (!gold) { printf("CUTSWEEP: context creation failed\n"); return; }
    if (!decode(gold, prompt.data(), P)) { printf("CUTSWEEP: prefill failed\n"); llama_free(gold); return; }
    std::vector<llama_token> seed = gen_greedy(gold, 16, vocab);
    llama_free(gold);

    char arch_buf[64] = {0};
    llama_model_meta_val_str(model, "general.architecture", arch_buf, sizeof(arch_buf));
    printf("CUTSWEEP %s | prompt=%d tok, n_rs_seq=%u, n_gen=8, axes: c (prefill tokens cut) x m (singles)\n"
           "  expectation: vanilla EXACT at m=0 / DIFFER at m>=1 (reads plane c+m; REFUSED once c+m > n_rs_seq);"
           " fix EXACT everywhere\n",
           arch_buf[0] ? arch_buf : "unknown-arch", P, kCutRsSeq);

    static const int cs[] = {1, 3, 8};
    static const int ms[] = {0, 1, 4};
    for (int c : cs) {
        for (int m : ms) {
            if (m > (int) seed.size() || c >= P) { continue; }
            std::vector<llama_token> sa, sb;
            const int rs = run_cut_cell(model, vocab, prompt, seed, c, m, false, sa, sb);
            std::string diff_note;
            if (rs == 1) {
                const int fd = first_diff(sa, sb);
                if (fd >= 0 && fd < (int) std::min(sa.size(), sb.size())) {
                    diff_note = "  first diff tok: A=\"" + detok(vocab, {sa[fd]}) + "\" B=\"" + detok(vocab, {sb[fd]}) + "\"";
                } else {
                    diff_note = "  streams differ in length (EOS)";
                }
            }
            const int rc = run_cut_cell(model, vocab, prompt, seed, c, m, true, sa, sb);
            printf("CELL c=%d m=%d : rollback %s | shape-control %s%s\n", c, m,
                   rs == 0 ? "EXACT" : rs == 1 ? "DIFFER" : rs == 3 ? "REFUSED" : "INFRA",
                   rc == 0 ? "SAME" : rc == 1 ? "NOISE" : "INFRA", diff_note.c_str());
            fflush(stdout);
        }
    }
}

// ---- dsteps: empirical restore map ------------------------------------------
// Static derivations of the snapshot-plane algebra kept getting falsified by
// measurements, so map the restore empirically instead: for each rollback depth
// d in 1..d_max a fresh A/B pair runs the identical feed, A rolls back and
// replays, and the next-token logits are compared bitwise over forced steps.
// The set of d that comes back exact reveals which temporal state the restore
// actually reads — no theory involved.
int run_statecmp(llama_model * model, const llama_vocab * vocab, int d, int m, const char * mode);

int run_dsteps(llama_model * model, const llama_vocab * vocab, int d_max, int m, const char * mode) {
    printf("DSTEPS %s m=%d | mapping restore exactness over d=1..%d\n", mode, m, d_max);
    fflush(stdout);
    for (int d = 1; d <= d_max; ++d) {
        const int rc = run_statecmp(model, vocab, d, m, mode);
        printf("  d=%2d -> %s\n", d,
               rc == 0 ? "EXACT" : rc == 6 ? "DIVERGE" : rc == 2 ? "REFUSED" : "INFRA");
        fflush(stdout);
    }
    return 0;
}

// ---- statecmp: bitwise logits probe -----------------------------------------
// Argmax-stream verdicts are coarse, and comparing serialized state blobs is
// confounded: the blob embeds all K snapshot planes whose WRITE HISTORIES
// legitimately differ between a rolled-back context and a never-rolled-back one,
// even when the restore is perfect. The clean signal is the next-token LOGITS:
// they depend only on the current state and never touch the snapshot planes.
// A rolls back + replays, B runs the same feed without the rollback; both are
// then stepped N times with identical forced tokens (the golden continuation),
// comparing the full logits row bitwise after every step. First mismatching
// step localizes the divergence; all-match means the restore is bitwise-exact.
int run_statecmp(llama_model * model, const llama_vocab * vocab, int d, int m, const char * mode) {
    const bool  single = strcmp(mode, "single") == 0;
    constexpr int n_steps = 4;
    const char * prompt_text =
        "The quick brown fox jumps over the lazy dog. Repeat after me, exactly and only: hello world\n";
    const std::vector<llama_token> prompt = tokenize(vocab, prompt_text);
    const int P = (int) prompt.size();

    // Golden chain on a snapshot-free context: m + d rm-phase tokens + n_steps forced steps.
    llama_context * gold = make_ctx(model, kNCtx, 0);
    if (!gold) { printf("STATECMP: context creation failed\n"); return 3; }
    if (!decode(gold, prompt.data(), P)) { printf("STATECMP: prefill failed\n"); llama_free(gold); return 3; }
    std::vector<llama_token> seed = gen_greedy(gold, m + d + n_steps, vocab);
    llama_free(gold);
    if ((int) seed.size() < m + d + n_steps) { printf("STATECMP: golden chain too short\n"); return 3; }

    llama_context * ctx_a = make_ctx(model, kNCtx, kNRsSeq);
    llama_context * ctx_b = make_ctx(model, kNCtx, kNRsSeq);
    if (!ctx_a || !ctx_b) { printf("STATECMP: context creation failed\n"); return 3; }

    int rc = 3;
    do {
        // B: same feed minus the replay (rollback only removes positions — no re-feed),
        // so A (rollback + replay) vs B (rm-phase feed) differ iff the restore itself differs;
        // d=0 (no rollback) must come back bitwise-identical — that's the soundness control
        if (!decode(ctx_b, prompt.data(), P)) { fprintf(stderr, "bmoe-rsbench: B prefill failed\n"); break; }
        bool ok = true;
        for (int i = 0; i < m && ok; ++i) { ok = decode(ctx_b, &seed[i], 1); }
        if (ok) {
            if (single) {
                for (int i = 0; i < d && ok; ++i) { ok = decode(ctx_b, &seed[m + i], 1); }
            } else {
                ok = decode(ctx_b, seed.data() + m, d);
            }
        }
        if (!ok) { fprintf(stderr, "bmoe-rsbench: B feed failed\n"); break; }

        // A: same feed, then the rollback + replay under test
        llama_memory_t mem_a = llama_get_memory(ctx_a);
        if (!decode(ctx_a, prompt.data(), P)) { fprintf(stderr, "bmoe-rsbench: A prefill failed\n"); break; }
        ok = true;
        for (int i = 0; i < m && ok; ++i) { ok = decode(ctx_a, &seed[i], 1); }
        if (ok) {
            if (single) {
                for (int i = 0; i < d && ok; ++i) { ok = decode(ctx_a, &seed[m + i], 1); }
            } else {
                ok = decode(ctx_a, seed.data() + m, d);
            }
        }
        if (!ok) { fprintf(stderr, "bmoe-rsbench: A feed failed\n"); break; }
        if (d > 0) {
            if (!llama_memory_seq_rm(mem_a, 0, P + m, -1)) {
                printf("STATECMP: seq_rm refused (depth %d > n_rs_seq?)\n", d);
                rc = 2;
                break;
            }
            if (!decode(ctx_a, seed.data() + m, d)) { fprintf(stderr, "bmoe-rsbench: A replay failed\n"); break; }
        } // d == 0: negative control — identical feeds, no rollback; must be bitwise-exact

        // N forced identical steps; logits must be bitwise-equal at every step.
        // The forced token is FIXED across depths (seed[0], not the timeline's next token)
        // so the per-step logits hashes are comparable across d runs — the whole point is
        // to cross-reference which temporal state the restore actually returns.
        printf("STATECMP %s d=%d m=%d | comparing logits bitwise over %d forced steps\n", mode, d, m, n_steps);
        int first_bad = -1;
        double max_diff = 0.0;
        for (int j = 0; j < n_steps; ++j) {
            const llama_token t = seed[0];
            if (!decode(ctx_a, &t, 1) || !decode(ctx_b, &t, 1)) { fprintf(stderr, "bmoe-rsbench: forced step failed (step %d)\n", j); ok = false; break; }
            const float * la = llama_get_logits_ith(ctx_a, 0);
            const float * lb = llama_get_logits_ith(ctx_b, 0);
            if (!la || !lb) { fprintf(stderr, "bmoe-rsbench: logits unavailable\n"); ok = false; break; }
            const int n_vocab = llama_vocab_n_tokens(vocab);
            const bool step_eq = memcmp(la, lb, (size_t) n_vocab * sizeof(float)) == 0;
            if (!step_eq) {
                // FNV-1a over the logits rows: lets us cross-reference A's restored state
                // against B's clean states at OTHER depths (is the restore a neighbor state?)
                uint64_t ha = 0, hb = 0;
                for (int v = 0; v < n_vocab; ++v) {
                    uint32_t xa, xb; memcpy(&xa, &la[v], 4); memcpy(&xb, &lb[v], 4);
                    ha = ha * 1099511628211ULL + xa; hb = hb * 1099511628211ULL + xb;
                }
                printf("  STEP %d hA=%016llx hB=%016llx DIFF\n", j, (unsigned long long) ha, (unsigned long long) hb);
            }
            if (!step_eq && first_bad < 0) {
                first_bad = j;
                for (int v = 0; v < n_vocab; ++v) { max_diff = std::max(max_diff, (double) std::fabs(la[v] - lb[v])); }
            }
        }
        if (!ok) { break; }
        if (first_bad < 0) {
            printf("  RESULT: BITWISE-EXACT — logits identical at all %d steps (restore is a clean time-shift)\n", n_steps);
            rc = 0;
        } else {
            printf("  RESULT: LOGITS DIVERGE at forced step %d (max |diff| %g) — restore is not bitwise\n", first_bad, max_diff);
            rc = 6;
        }
    } while (false);

    llama_free(ctx_a);
    llama_free(ctx_b);
    return rc;
}

} // namespace

int main(int argc, char ** argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: bmoe-rsbench reserve|diverge <model.gguf> [n_gen] | sweep <model.gguf> [fox]\n"
                "       bmoe-rsbench statecmp <model.gguf> [d=8] [m=0] [single|batched]\n"
                "       bmoe-rsbench dsteps <model.gguf> [d_max=8] [m=0] [single|batched]\n"
                "       bmoe-rsbench cutsweep <model.gguf>   (cut-into-prefill cells + shape-noise controls)\n");
        return 1;
    }
    const char * mode       = argv[1];
    const char * model_path = argv[2];
    int n_gen = argc > 3 ? atoi(argv[3]) : 24;

    llama_model * model = nullptr;
    const llama_vocab * vocab = nullptr;
    model = load_model(model_path, vocab);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    if (strcmp(mode, "reserve") == 0) {
        llama_context * ctx = make_ctx(model, 2048, 64);
        if (!ctx) { printf("RESERVE: FAIL — context creation returned null\n"); return 3; }
        std::vector<llama_token> toks = tokenize(vocab, "hello");
        llama_batch batch = llama_batch_get_one(toks.data(), (int)toks.size());
        if (llama_decode(ctx, batch)) {
            printf("RESERVE: CRASH or decode failure at first decode (see stderr above)\n");
            return 4;
        }
        printf("RESERVE: OK — n_rs_seq=64 context created and decoded\n");
        llama_free(ctx);
        llama_model_free(model);
        return 0;
    }

    if (strcmp(mode, "diverge") == 0) {
        // Fixed cell: single-mode rm phase, d_rm=3 (the fixture-test depth), m=0.
        // Reference = an identically-fed context that never rolls back (matched shape —
        // the old version compared against a fresh-context decode with different memory history).
        const char * prompt_text =
            "The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. "
            "Repeat after me, exactly and only: hello world\n";
        const std::vector<llama_token> prompt = tokenize(vocab, prompt_text);
        std::vector<llama_token> seed(4 + 3 + 128);
        Cell c = {"single", 3, 0};
        std::vector<llama_token> sa, sb;
        // seed chain: reuse the golden from a no-snapshot context
        llama_context * gold = make_ctx(model, kNCtx, 0);
        if (!gold) { printf("DIVERGE: context creation failed\n"); return 3; }
        decode(gold, prompt.data(), (int)prompt.size());
        seed = gen_greedy(gold, (int)seed.size(), vocab);
        llama_free(gold);
        const int rc = run_cell(model, vocab, prompt, seed, c, true, sa, sb);
        if (rc == 2) { printf("DIVERGE: infrastructure failure\n"); return 4; }
        printf("DIVERGE: %s\n", rc == 0 ? "EXACT" : "DIFFER");
        printf("  restored : \"%s\"\n", detok(vocab, sa).c_str());
        printf("  reference: \"%s\"\n", detok(vocab, sb).c_str());
        llama_model_free(model);
        return rc == 0 ? 0 : 5;
    }

    if (strcmp(mode, "sweep") == 0) {
        run_sweep(model, vocab, argc > 3 && strcmp(argv[3], "fox") == 0);
        llama_model_free(model);
        return 0;
    }

    if (strcmp(mode, "cutsweep") == 0) {
        run_cut_sweep(model, vocab);
        llama_model_free(model);
        return 0;
    }

    if (strcmp(mode, "statecmp") == 0) {
        const int      d  = argc > 3 ? atoi(argv[3]) : 8;
        const int      m  = argc > 4 ? atoi(argv[4]) : 0;
        const char * cm = argc > 5 ? argv[5] : "single";
        const int rc = run_statecmp(model, vocab, d, m, cm);
        llama_model_free(model);
        return rc;
    }

    if (strcmp(mode, "dsteps") == 0) {
        const int      dmax = argc > 3 ? atoi(argv[3]) : 8;
        const int      m    = argc > 4 ? atoi(argv[4]) : 0;
        const char * cm    = argc > 5 ? argv[5] : "single";
        run_dsteps(model, vocab, dmax, m, cm);
        llama_model_free(model);
        return 0;
    }

    fprintf(stderr, "bmoe-rsbench: unknown mode\n");
    return 1;
}
