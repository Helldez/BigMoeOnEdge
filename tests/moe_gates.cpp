// Byte-identity gates for MoE expert streaming.
//
// Greedy generation is a deterministic function of the graph, so streaming only the
// routed experts must produce output identical to running with every expert resident.
// These gates assert exactly that on the tiny synthetic model (scripts/make-tiny-moe.py),
// which the test harness generates first. Pass the model path as argv[1].
//
//   G1  resident (no streaming)        == streaming, cache off
//   G2  streaming, cache off           == streaming, small LRU cache (forces evictions)
//   G3  streaming, selective           == streaming, --load-all (every expert each token)
//   G6  resident                       == streaming + --dense-odirect (dense weights rebound to
//                                          O_DIRECT anon buffers — the rebind must be byte-identical)
//   G7  resident                       == streaming + --dense-odirect with O_DIRECT off (the rebind
//                                          is byte-identical whether or not the read bypasses the cache)
//   G4  overlap (async reads + per-expert wait hook) == serial streaming, cache off
//         a) overlap, cache off              b) overlap, small forced cache
//         c) overlap, cache off, io_threads=1 (single lane → maximal stalls)
//
// If G3 passes, the streamer provably never gathers an unrouted (garbage) slice. If G4
// passes, the async path gates each expert correctly — compute never races ahead of its read.
// G4 is compiled only when the fork's expert-ready hook is present (BMOE_HAVE_EXPERT_READY_HOOK).
//
//   S1  two sequential Session generates (warm cache) == one-shot resident, per prompt
//   S3  same, with an explicit budget shrink between the two generates (full eviction pass)
//   S2  same as S1, under overlap
//   G5  streaming + temporal prefetch == streaming (prefetch changes latency, not bytes)
//         a) serial + cache + prefetch   b) overlap + cache + prefetch
//
// S1/S2 guard the session refactor: the expert LRU cache now survives across generate() calls,
// so a second prompt starts warm. That must change only latency, never the produced bytes.
// S3 guards set_cache_budget_mb: evicting a warm cache mid-session must cost only re-reads.
// G5 guards temporal prefetch: speculatively reading the next layers' experts must only warm the
// cache — the routed slices a token actually consumes, and thus its output, are unchanged.
//
//   G9  streaming + --predict-log == streaming (the prediction probe observes and nothing more),
//       and its zero-staleness control reproduces the router it measures
//
// G9's second half is the probe measuring itself: the control shares every line of code with the
// prediction under test and differs only in having no staleness, so it must agree with llama.cpp's
// own routing. A probe that is quietly wrong would otherwise report a plausible number.
//
//   G10 streaming + --predict-prefetch == streaming (speculating on the prediction only warms the
//       cache), and the run must actually have speculated — an inert predictor passes vacuously.
//   G8  streaming + --drop-cold-experts, plumbing vs policy: an inert threshold drops nothing, the
//       top-weighted expert is never dropped, and full strength survives a constantly evicting cache
//   G13 the speculative decode loop, via the n-gram source on a prompt with nothing to match: it
//       abstains, so every step takes the plain path and the output must be identical
//   G14 --route-ahead, in two halves: a horizon past every layer overrides nothing and must be
//       identical, and a horizon of one must commit real routings and still generate
////   G15 --row-stream, the dense tables the graph only gathers rows from: a) served from flash
//       inside the default window and b) inside a window of one slab, so nearly every gather
//       evicts what the last one fetched. Both must be byte-identical to the resident reference.
//
// G15 needs no separate "did it do anything" check of the G10 kind: the tensor is bound to
// RESERVED address space, so a row the policy fails to fetch is not a slightly wrong weight but
// memory that was never written, and the output diverges on the first token. Identity is proof
// that every gathered row was fetched. What it cannot prove is that a table qualified at all —
// on a tiny model with tied embeddings none would, and the gate would pass vacuously. That is
// what the run's own "moe-rows:" line reports, on the real model.
//
// Gate numbers are allocated once and never reused: a failing label has to name one thing. G11 and
// G12 are reserved for the zero-copy branch (PR #143) and must not be taken here.
#include "bmoe/config.h"
#include "bmoe/runtime.h"
#include "bmoe/session.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace bmoe;

static RunConfig base(const std::string & model) {
    RunConfig c;
    c.model_path = model;
    c.prompt = "Hello world, this is a streaming test.";
    c.n_predict = 24;
    c.n_threads = 2;
    c.n_ctx = 256;
    return c;
}

static bool gen(const RunConfig & c, std::string & out, std::string & err) {
    RunResult r = run(c);
    if (!r) {
        err = r.error;
        return false;
    }
    out = r.generated_text;
    return true;
}

// Open a Session from a RunConfig and generate the same prompt twice. The second generate runs
// against the cache the first left warm — the whole point of session mode — so its output is the
// interesting one: it must still match the cold one-shot reference.
static bool session_two_gens(const RunConfig & c, std::string & out1, std::string & out2, std::string & err) {
    SessionConfig sc;
    sc.model_path = c.model_path;
    sc.n_threads = c.n_threads;
    sc.n_ctx = c.n_ctx;
    sc.n_batch = c.n_ctx;
    sc.chatml = c.chatml;
    sc.moe = c.moe;

    std::unique_ptr<Session> s = Session::open(sc, err);
    if (!s) return false;

    GenerateRequest req;
    req.prompt = c.prompt;
    req.n_predict = c.n_predict;
    req.clear_kv = true;

    RunResult r1 = s->generate(req);
    if (!r1) {
        err = r1.error;
        return false;
    }
    RunResult r2 = s->generate(req);
    if (!r2) {
        err = r2.error;
        return false;
    }
    out1 = r1.generated_text;
    out2 = r2.generated_text;
    return true;
}

// Open a Session (cache on), generate, shrink the cache budget hard between generations to force a
// mass eviction of the warm cache, then generate again. The second output must still match the cold
// resident reference: a runtime resize only changes residency, never the produced bytes.
static bool
session_shrink_gen(const RunConfig & c, int shrink_mib, std::string & out1, std::string & out2, std::string & err) {
    SessionConfig sc;
    sc.model_path = c.model_path;
    sc.n_threads = c.n_threads;
    sc.n_ctx = c.n_ctx;
    sc.n_batch = c.n_ctx;
    sc.chatml = c.chatml;
    sc.moe = c.moe;

    std::unique_ptr<Session> s = Session::open(sc, err);
    if (!s) return false;

    GenerateRequest req;
    req.prompt = c.prompt;
    req.n_predict = c.n_predict;
    req.clear_kv = true;

    RunResult r1 = s->generate(req);
    if (!r1) {
        err = r1.error;
        return false;
    }
    s->set_cache_budget_mb(shrink_mib); // between generations (no decode in flight): evicts to budget
    RunResult r2 = s->generate(req);
    if (!r2) {
        err = r2.error;
        return false;
    }
    out1 = r1.generated_text;
    out2 = r2.generated_text;
    return true;
}

static int check(const char * name, const std::string & a, const std::string & b) {
    if (a == b) {
        std::printf("[PASS] %s\n", name);
        return 0;
    }
    std::printf("[FAIL] %s\n  A: %s\n  B: %s\n", name, a.c_str(), b.c_str());
    return 1;
}

int main(int argc, char ** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <tiny-moe.gguf>\n", argv[0]);
        return 2;
    }
    const std::string model = argv[1];

    // resident reference
    RunConfig resident = base(model);
    resident.moe.enabled = false;

    // streaming, cache off
    RunConfig stream0 = base(model);
    stream0.moe.enabled = true;
    stream0.moe.cache_mb = 0;
    stream0.moe.io_threads = 4;

    // streaming, small LRU cache (pathological band → force it on for the test)
    RunConfig streamc = base(model);
    streamc.moe.enabled = true;
    streamc.moe.cache_mb = 2;
    streamc.moe.force_cache = true;
    streamc.moe.io_threads = 4;

    // streaming, load-all baseline
    RunConfig streamall = base(model);
    streamall.moe.enabled = true;
    streamall.moe.cache_mb = 0;
    streamall.moe.load_all = true;

    // streaming, cache off, dense weights read via O_DIRECT into anon buffers and rebound (the
    // Anonymous policy, which does not warm — so the gate exercises the rebind alone: the rebound
    // bytes must equal the mmap reference).
    RunConfig dense_od = base(model);
    dense_od.moe.enabled = true;
    dense_od.moe.cache_mb = 0;
    dense_od.moe.io_threads = 4;
    dense_od.moe.dense_weights = DenseWeightsMode::Anonymous;

    // Same, but with the expert stream's O_DIRECT off: the dense reader still bypasses the cache on
    // its own choice, so this proves the rebind is byte-identical regardless of the expert flag.
    RunConfig dense_od_buf = dense_od;
    dense_od_buf.moe.o_direct = false;

    // Row-gathered dense tables served from flash instead of RAM. The table is bound to reserved
    // address space, so a row the policy failed to fetch is not a slightly wrong weight — it is
    // unwritten memory, and the output diverges immediately. That makes byte-identity the whole
    // test: G15a covers the ordinary path, G15b the eviction path, with a window of one slab so that
    // almost every gather has to re-read what the previous one just handed back.
    RunConfig rows = base(model);
    rows.moe.enabled = true;
    rows.moe.cache_mb = 0;
    rows.moe.io_threads = 4;
    rows.moe.row_stream = true;

    RunConfig rows_tight = rows;
    rows_tight.moe.row_stream_mb = 0; // clamped to a single slab: maximum eviction pressure

    std::string s_res, s_s0, s_sc, s_all, s_dod, s_dodb, err;
    if (!gen(resident, s_res, err)) {
        std::fprintf(stderr, "resident run failed: %s\n", err.c_str());
        return 2;
    }
    if (!gen(stream0, s_s0, err)) {
        std::fprintf(stderr, "stream0 run failed: %s\n", err.c_str());
        return 2;
    }
    if (!gen(streamc, s_sc, err)) {
        std::fprintf(stderr, "streamc run failed: %s\n", err.c_str());
        return 2;
    }
    if (!gen(streamall, s_all, err)) {
        std::fprintf(stderr, "load-all run failed: %s\n", err.c_str());
        return 2;
    }
    if (!gen(dense_od, s_dod, err)) {
        std::fprintf(stderr, "dense-odirect run failed: %s\n", err.c_str());
        return 2;
    }
    if (!gen(dense_od_buf, s_dodb, err)) {
        std::fprintf(stderr, "dense-odirect(no O_DIRECT) run failed: %s\n", err.c_str());
        return 2;
    }
    std::string s_rows, s_rows_tight;
    if (!gen(rows, s_rows, err)) {
        std::fprintf(stderr, "row-stream run failed: %s\n", err.c_str());
        return 2;
    }
    if (!gen(rows_tight, s_rows_tight, err)) {
        std::fprintf(stderr, "row-stream(one-slab window) run failed: %s\n", err.c_str());
        return 2;
    }

    int fails = 0;
    fails += check("G1 resident == streaming(cache off)", s_res, s_s0);
    fails += check("G2 streaming(cache off) == streaming(LRU cache)", s_s0, s_sc);
    fails += check("G3 streaming(selective) == streaming(load-all)", s_s0, s_all);
    // G6: rebinding the dense weights onto O_DIRECT anon buffers must not change a byte — same
    // bytes, same offsets, so identical output to the mmap-resident reference. This is the
    // correctness proof for --dense-odirect (the risk is rebinding the wrong tensor).
    fails += check("G6 dense-odirect(rebind) == resident", s_res, s_dod);
    // G7: the rebind is byte-identical whether or not the dense read bypassed the page cache.
    fails += check("G7 dense=anon + expert O_DIRECT off == resident", s_res, s_dodb);
    fails += check("G15a row-stream(row-gathered tables from flash) == resident", s_res, s_rows);
    fails += check("G15b row-stream(one-slab window, evicting) == resident", s_res, s_rows_tight);

#ifdef BMOE_HAVE_EXPERT_READY_HOOK
    // overlap, cache off
    RunConfig ov0 = base(model);
    ov0.moe.enabled = true;
    ov0.moe.cache_mb = 0;
    ov0.moe.io_threads = 4;
    ov0.moe.overlap = true;

    // overlap, small forced cache (same cache config as G2's streamc)
    RunConfig ovc = base(model);
    ovc.moe.enabled = true;
    ovc.moe.cache_mb = 2;
    ovc.moe.force_cache = true;
    ovc.moe.io_threads = 4;
    ovc.moe.overlap = true;

    // overlap, cache off, single I/O lane (stress: the compute threads stall on every expert)
    RunConfig ov1 = base(model);
    ov1.moe.enabled = true;
    ov1.moe.cache_mb = 0;
    ov1.moe.io_threads = 1;
    ov1.moe.overlap = true;

    // overlap, cache, two-wave publish: the lanes wake on the first projection's jobs and the
    // batch grows mid-generation. Exercises the grown-batch worker predicate and the split
    // commit; the wait-per-expert hook must still gate every projection to the same bytes.
    RunConfig ov2w = base(model);
    ov2w.moe.enabled = true;
    ov2w.moe.cache_mb = 2;
    ov2w.moe.force_cache = true;
    ov2w.moe.io_threads = 4;
    ov2w.moe.overlap = true;
    ov2w.moe.io_two_wave = true;

    std::string s_ov0, s_ovc, s_ov1, s_ov2w;
    if (!gen(ov0, s_ov0, err)) {
        std::fprintf(stderr, "overlap(cache off) run failed: %s\n", err.c_str());
        return 2;
    }
    if (!gen(ovc, s_ovc, err)) {
        std::fprintf(stderr, "overlap(cache) run failed: %s\n", err.c_str());
        return 2;
    }
    if (!gen(ov1, s_ov1, err)) {
        std::fprintf(stderr, "overlap(io_threads=1) run failed: %s\n", err.c_str());
        return 2;
    }
    if (!gen(ov2w, s_ov2w, err)) {
        std::fprintf(stderr, "overlap(two-wave) run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("G4a overlap(cache off) == streaming(cache off)", s_s0, s_ov0);
    fails += check("G4b overlap(LRU cache) == streaming(cache off)", s_s0, s_ovc);
    fails += check("G4c overlap(io_threads=1) == streaming(cache off)", s_s0, s_ov1);
    fails += check("G4d overlap(two-wave publish) == streaming(cache off)", s_s0, s_ov2w);
#else
    std::printf("[SKIP] G4 (expert-ready hook not built)\n");
#endif

    // ── S1/S2: warm cache across Session generates must not change bytes ──
    // A forced small LRU cache so the first generate leaves state (resident + evicted entries)
    // the second one reuses — exercising the "starts warm" path, not a cold re-run.
    RunConfig sess = base(model);
    sess.moe.enabled = true;
    sess.moe.cache_mb = 2;
    sess.moe.force_cache = true;
    sess.moe.io_threads = 4;

    std::string s_g1, s_g2;
    if (!session_two_gens(sess, s_g1, s_g2, err)) {
        std::fprintf(stderr, "session run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("S1a session generate #1 == resident", s_res, s_g1);
    fails += check("S1b session generate #2 (warm cache) == resident", s_res, s_g2);

    // S3: an explicit runtime budget change (set_cache_budget_mb, as an app's memory-pressure callback
    // makes it) evicts warm entries mid-session; the next generation must rebuild them from flash
    // byte-for-byte. Reuse the small forced cache, then drop it to ~0 MiB between generates to force a
    // full eviction pass.
    std::string s_sh1, s_sh2;
    if (!session_shrink_gen(sess, /*shrink_mib=*/0, s_sh1, s_sh2, err)) {
        std::fprintf(stderr, "session shrink run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("S3a session generate #1 == resident", s_res, s_sh1);
    fails += check("S3b session generate #2 (after budget shrink) == resident", s_res, s_sh2);

    // ── G5: temporal prefetch must not change bytes ──
    // A forced cache (prefetch needs one) plus a couple of look-ahead layers, exercising the
    // speculative queue, integration and eviction against the plain streamed reference.
    RunConfig pf = base(model);
    pf.moe.enabled = true;
    pf.moe.cache_mb = 2;
    pf.moe.force_cache = true;
    pf.moe.io_threads = 4;
    pf.moe.prefetch_layers = 2;
    std::string s_pf;
    if (!gen(pf, s_pf, err)) {
        std::fprintf(stderr, "prefetch run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("G5a streaming(prefetch) == streaming(cache off)", s_s0, s_pf);

#ifdef BMOE_HAVE_EXPERT_READY_HOOK
    RunConfig pf_ov = pf;
    pf_ov.moe.overlap = true;
    std::string s_pf_ov;
    if (!gen(pf_ov, s_pf_ov, err)) {
        std::fprintf(stderr, "prefetch overlap run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("G5b overlap(prefetch) == streaming(cache off)", s_s0, s_pf_ov);
#else
    std::printf("[SKIP] G5b (expert-ready hook not built)\n");
#endif

    // G5c forces speculative reads to complete synchronously, so the integrate-then-hit path (a
    // prefetched expert becoming resident and a later routing hitting it) is deterministically
    // exercised — the timing race in G5a/b rarely reaches it on a fast host.
    RunConfig pf_sync = pf;
    pf_sync.moe.prefetch_sync = true;
    std::string s_pf_sync;
    if (!gen(pf_sync, s_pf_sync, err)) {
        std::fprintf(stderr, "prefetch(sync) run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("G5c prefetch(sync integrate+hit) == streaming(cache off)", s_s0, s_pf_sync);

#ifdef BMOE_HAVE_EXPERT_READY_HOOK
    RunConfig sess_ov = sess;
    sess_ov.moe.overlap = true;
    std::string s_og1, s_og2;
    if (!session_two_gens(sess_ov, s_og1, s_og2, err)) {
        std::fprintf(stderr, "session overlap run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("S2a session+overlap generate #1 == resident", s_res, s_og1);
    fails += check("S2b session+overlap generate #2 (warm cache) == resident", s_res, s_og2);
#else
    std::printf("[SKIP] S2 (expert-ready hook not built)\n");
#endif

    // G8 — cache-aware expert dropping, plumbing vs policy.
    //
    // Arming the policy moves load_layer() from the topk node to the terminal node of the layer's
    // weight chain, and has the hook learn which node that is. That machinery must be transparent:
    // with a threshold below any weight the router can produce, nothing is dropped and the output
    // must stay byte-identical to the undropped stream. This separates "the deferral is correct"
    // from "the policy is lossy" — only the second is allowed to change bytes, and a regression in
    // the first would otherwise hide behind the expected difference.
    // The policy needs a real LRU cache: with the cache off every expert reads as a miss, so it
    // would degenerate into an unconditional weight cut and the repointing below would never face
    // the reserved-but-uncommitted slot it exists to avoid. The small forced budget is the same one
    // G2 uses to provoke evictions, so misses and hits both occur.
    RunConfig drop_inert = base(model);
    drop_inert.moe.enabled = true;
    drop_inert.moe.cache_mb = 2;
    drop_inert.moe.force_cache = true;
    drop_inert.moe.io_threads = 4;
    drop_inert.moe.drop_cold_frac = 1e-6f;
    std::string s_drop_inert;
    if (!gen(drop_inert, s_drop_inert, err)) {
        std::fprintf(stderr, "drop(inert threshold) run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("G8a drop(threshold below any weight) == streaming(cached, undropped)", s_sc, s_drop_inert);
    // The identity above only means anything if the policy really was armed and really dropped
    // nothing. Asserting the count separately turns "a weight happened to fall under the threshold"
    // from a mysterious byte mismatch into a legible failure.
    {
        RunResult r = run(drop_inert);
        if (!r || r.summary.experts_dropped != 0 || r.summary.experts_routed <= 0) {
            std::printf("[FAIL] G8a' inert threshold must examine routings and drop none (routed=%lld dropped=%lld)\n",
                        r.summary.experts_routed, r.summary.experts_dropped);
            ++fails;
        } else {
            std::printf("[PASS] G8a' inert threshold examined %lld routings, dropped none\n", r.summary.experts_routed);
        }
    }

    // G8b — the same at full strength, against a cache small enough to be evicting constantly, so
    // dropped experts really do land on slots the cache has released. There is no reference output
    // to compare against (it is lossy by design), so the gate is that the engine survives it: a
    // dropped expert's slot is repointed at one that is certainly resident, so the matmul must never
    // read reserved-but-uncommitted memory and generation must still complete.
    RunConfig drop_hard = drop_inert;
    drop_hard.moe.drop_cold_frac = 1.0f;
    drop_hard.moe.drop_prefill = true;
    std::string s_drop_hard;
    if (!gen(drop_hard, s_drop_hard, err)) {
        std::fprintf(stderr, "drop(full strength) run failed: %s\n", err.c_str());
        return 2;
    }
    if (s_drop_hard.empty()) {
        std::printf("[FAIL] G8b drop(full strength) produced no output\n");
        ++fails;
    } else {
        std::printf("[PASS] G8b drop(full strength) generates without touching unloaded experts\n");
    }

    // G8c — the top-weighted expert is pinned, so a routing can never be emptied. Forcing top-k to
    // 1 makes every routed expert the top one, and dropping must then be a no-op at ANY threshold:
    // the output has to match the same k=1 run with the policy off, byte for byte. This also pins
    // down that the threshold is taken against the EFFECTIVE top-k discovered at runtime — a
    // hardcoded width would not survive the override.
    RunConfig k1 = base(model);
    k1.moe.enabled = true;
    k1.moe.cache_mb = 2;
    k1.moe.force_cache = true;
    k1.moe.io_threads = 4;
    k1.n_expert_used = 1;
    RunConfig k1_drop = k1;
    k1_drop.moe.drop_cold_frac = 1.0f;
    k1_drop.moe.drop_prefill = true;
    std::string s_k1, s_k1_drop;
    if (!gen(k1, s_k1, err) || !gen(k1_drop, s_k1_drop, err)) {
        std::fprintf(stderr, "top-k=1 drop run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("G8c drop(full strength, top-k=1) == top-k=1 undropped (top expert pinned)", s_k1, s_k1_drop);

    // G8d — cache-aware substitution with a margin too small to move any routing must be
    // byte-identical to the same cached run, and must have examined routings while changing none.
    // Like G8a it runs against the small forced budget, so residency is a real mix of hits and
    // misses and the re-ranking has something to prefer — it just may not prefer it at this margin.
    RunConfig sub_inert = base(model);
    sub_inert.moe.enabled = true;
    sub_inert.moe.cache_mb = 2;
    sub_inert.moe.force_cache = true;
    sub_inert.moe.io_threads = 4;
    sub_inert.moe.substitute_lambda = 1e-6f;
    std::string s_sub_inert;
    if (!gen(sub_inert, s_sub_inert, err)) {
        std::fprintf(stderr, "substitute(inert margin) run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("G8d substitute(margin below any gap) == streaming(cached, unsubstituted)", s_sc, s_sub_inert);
    {
        RunResult r = run(sub_inert);
        if (!r || r.summary.experts_substituted != 0 || r.summary.experts_reranked <= 0) {
            std::printf("[FAIL] G8d' inert margin must examine routings and substitute none (reranked=%lld "
                        "substituted=%lld)\n",
                        r.summary.experts_reranked, r.summary.experts_substituted);
            ++fails;
        } else {
            std::printf("[PASS] G8d' inert margin examined %lld slots, substituted none\n", r.summary.experts_reranked);
        }
    }

    // G8e — at the full margin a resident expert outranks every non-resident one, so against a
    // cache that holds some of the layer the re-ranking MUST fire. There is no reference output (it
    // is lossy by design); the gate is that generation completes and that the policy demonstrably
    // acted: a count of zero here would mean the flag is inert, which is the failure mode that a
    // lossy knob with a plausible-looking output can hide indefinitely.
    RunConfig sub_full = sub_inert;
    sub_full.moe.substitute_lambda = 1.0f;
    {
        RunResult r = run(sub_full);
        if (!r || r.summary.experts_substituted <= 0 || r.generated_text.empty()) {
            std::printf("[FAIL] G8e substitute(full margin) must generate and substitute (reranked=%lld "
                        "substituted=%lld, %zu bytes)\n",
                        r ? r.summary.experts_reranked : 0LL, r ? r.summary.experts_substituted : 0LL,
                        r ? r.generated_text.size() : (size_t) 0);
            ++fails;
        } else {
            std::printf("[PASS] G8e substitute(full margin) generated, %lld/%lld reranked slots went to a resident "
                        "expert\n",
                        r.summary.experts_substituted, r.summary.experts_reranked);
        }
    }

    // G9 — the expert-prediction probe observes and nothing more.
    //
    // It isolates an extra node per layer and reads the router's inputs, which puts it inside the
    // most load-bearing callback in the engine. So the first thing to pin down is that it changes
    // nothing: same prompt, same bytes as the unprobed stream.
    RunConfig probe = base(model);
    probe.moe.enabled = true;
    probe.moe.cache_mb = 0;
    probe.moe.predict_log = true;
    std::string s_probe;
    if (!gen(probe, s_probe, err)) {
        std::fprintf(stderr, "predict-log run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("G9a predict-log == streaming(cache off) (the probe only observes)", s_s0, s_probe);

    // G9b — and that it measured something rather than reporting an empty run as a clean one.
    //
    // The load-bearing assertion is the CONTROL. It ranks experts from the layer's own gate matrix
    // and its own gate input — the same row read, GEMV and ranking the stale prediction uses, with
    // only the staleness removed — so it has to reproduce the selection llama.cpp computed from
    // those exact two tensors. A transposed matrix, a mis-strided row or a wrong token would leave
    // it near chance while the stale number stayed superficially plausible. It is checked against
    // 0.99 rather than 1.0 because the probe accumulates in scalar float where ggml does not, so a
    // routing whose top-k straddles a near-tie may legitimately order two experts differently.
    {
        RunResult r = run(probe);
        const PredictorStats & self = r.summary.predict_self;
        const PredictorStats & stale = r.summary.predict_stale;
        const PredictorStats & prev = r.summary.predict_prev;
        if (!r || self.rows == 0 || stale.rows == 0 || prev.rows == 0) {
            std::printf("[FAIL] G9b probe scored nothing (self=%lld stale=%lld prev=%lld routings)\n", self.rows,
                        stale.rows, prev.rows);
            ++fails;
        } else if (self.hit_frac() < 0.99) {
            std::printf("[FAIL] G9b zero-staleness control only %.1f%% — the probe's own gate arithmetic disagrees "
                        "with the router it is measuring\n",
                        100.0 * self.hit_frac());
            ++fails;
        } else {
            std::printf("[PASS] G9b control %.1f%% over %lld routings (stale %.1f%%, prev-token %.1f%%)\n",
                        100.0 * self.hit_frac(), self.rows, 100.0 * stale.hit_frac(), 100.0 * prev.hit_frac());
        }
        // The per-layer breakdown is the half of the report conclusions get drawn from, so it must
        // exist and the three predictors must be indexed the same way — a table where one of them
        // is shifted or short would compare a layer against a different layer, legibly but wrongly.
        const size_t nst = r.summary.predict_stale_by_layer.size();
        if (r && (nst == 0 || r.summary.predict_prev_by_layer.size() != nst ||
                  r.summary.predict_self_by_layer.size() != nst)) {
            std::printf("[FAIL] G9b per-layer tables disagree (stale=%d prev=%d self=%d)\n", (int) nst,
                        (int) r.summary.predict_prev_by_layer.size(), (int) r.summary.predict_self_by_layer.size());
            ++fails;
        }
    }

    // G10 — predictive prefetch is speculation and nothing more.
    //
    // Same contract as the temporal prefetch (G5): whatever the predictor speculates, the routed
    // slices a token consumes — and thus its output — must not change. prefetch-sync completes the
    // speculative reads deterministically so the integrate-then-hit path is actually exercised on
    // a fast host, exactly as G5c does; the small forced cache makes hits and evictions both occur.
    RunConfig ppf = base(model);
    ppf.moe.enabled = true;
    ppf.moe.cache_mb = 2;
    ppf.moe.force_cache = true;
    ppf.moe.io_threads = 4;
    ppf.moe.predict_prefetch = true;
    ppf.moe.prefetch_sync = true;
    std::string s_ppf;
    if (!gen(ppf, s_ppf, err)) {
        std::fprintf(stderr, "predict-prefetch run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("G10a predict-prefetch(sync) == streaming(cache off)", s_s0, s_ppf);
    // The identity is only evidence if something was actually speculated: a predictor that never
    // fired would pass G10a vacuously. spec_experts counts what the speculative path fully read.
    {
        RunResult r = run(ppf);
        if (!r || r.summary.moe_spec_experts <= 0) {
            std::printf("[FAIL] G10b predict-prefetch speculated nothing (spec_experts=%lld)\n",
                        r.summary.moe_spec_experts);
            ++fails;
        } else {
            std::printf("[PASS] G10b predict-prefetch speculated %lld experts, %lld useful (%.0f%%)\n",
                        r.summary.moe_spec_experts, r.summary.moe_spec_useful,
                        r.summary.moe_spec_experts > 0 ? 100.0 * r.summary.moe_spec_useful / r.summary.moe_spec_experts
                                                       : 0.0);
        }
    }

    // G13 — the speculative decode loop, gated for real rather than declared untestable.
    //
    // Speculation is documented as not byte-identical, and for the MTP head that is true: its verify
    // decode evaluates several positions in one batch, and a batched matmul is not bit-identical to
    // that many single-token ones, so a near-tie can flip. The N-GRAM source escapes that on a
    // prompt with nothing to match. It abstains, every step takes the plain one-token path, and the
    // output must therefore equal the unspeculated run EXACTLY. That is a genuine gate over the
    // whole loop: the wider batch is built, the draft source is asked, the accept pass runs, the KV
    // is rewound at the end of the turn. Those are ~240 lines that touch the KV cache and had no
    // runtime coverage at all before this.
    // Abstention is forced rather than assumed. The first draft of this gate left the matcher at its
    // default and expected a synthetic model's output to repeat nothing; it drafted anyway (20
    // decodes for 24 tokens). The identity still held, but a gate must not rest on that: a verify
    // batch evaluates several positions at once, and the docs are explicit that a near-tie can then
    // order differently. Requiring a match longer than the whole generation makes the source unable
    // to fire, so the plain path is taken by construction and the identity is structural.
    RunConfig spec_ngram = base(model);
    spec_ngram.moe.enabled = true;
    spec_ngram.moe.cache_mb = 0;
    spec_ngram.moe.io_threads = 4;
    spec_ngram.spec.source = DraftSource::ngram;
    spec_ngram.spec.draft_max = 3;
    // Both bounds, because validate() rejects a floor above the longest suffix considered. A match
    // this long cannot exist inside a generation this short, so the source can never fire.
    spec_ngram.spec.ngram_max_match = SpecConfig::ngram_match_limit;
    spec_ngram.spec.ngram_min_match = SpecConfig::ngram_match_limit;
    std::string s_spec_ngram;
    if (!gen(spec_ngram, s_spec_ngram, err)) {
        std::fprintf(stderr, "ngram speculative run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("G13a ngram speculation (forced to abstain) == streaming(cache off)", s_s0, s_spec_ngram);
    // And the loop really ran, rather than the source being disabled somewhere and the identity
    // holding for the boring reason. One verify decode per generated token, and zero drafting steps,
    // is exactly what a source that cannot fire produces.
    {
        RunResult r = run(spec_ngram);
        if (!r || r.summary.mtp_decodes != r.summary.n_generated || r.summary.drafted_steps != 0) {
            std::printf("[FAIL] G13b forced-abstain ngram must cost one decode per token and draft none "
                        "(decodes=%lld tokens=%d drafted=%lld)\n",
                        r ? r.summary.mtp_decodes : -1, r ? r.summary.n_generated : -1,
                        r ? r.summary.drafted_steps : -1);
            ++fails;
        } else {
            std::printf("[PASS] G13b forced-abstain ngram ran the verify loop at one decode per token (%lld)\n",
                        r.summary.mtp_decodes);
        }
    }

    // G13c — and the loop with drafting ACTUALLY ON. There is no reference output here (a batched
    // verify may order a near-tie differently), so the assertion is the one G8b makes for the lossy
    // drop policy: the machinery survives. Drafts are proposed, a wider batch is decoded, a prefix
    // is accepted, the rest is rolled back and the KV is trimmed at the end of the turn. A mistake
    // in any of those corrupts the context rather than the arithmetic, and shows up as empty or
    // truncated output.
    RunConfig spec_live = spec_ngram;
    spec_live.spec.ngram_min_match = 1; // fire on the shortest possible match, so it drafts often
    spec_live.spec.ngram_max_match = 12;
    std::string s_spec_live;
    if (!gen(spec_live, s_spec_live, err)) {
        std::fprintf(stderr, "ngram speculative (drafting) run failed: %s\n", err.c_str());
        return 2;
    }
    {
        RunResult r = run(spec_live);
        if (!r || s_spec_live.empty() || r.summary.drafted_steps <= 0 ||
            r.summary.mtp_decodes >= r.summary.n_generated) {
            std::printf("[FAIL] G13c drafting ngram must draft, amortise decodes and still generate "
                        "(drafted=%lld decodes=%lld tokens=%d empty=%d)\n",
                        r ? r.summary.drafted_steps : -1, r ? r.summary.mtp_decodes : -1,
                        r ? r.summary.n_generated : -1, (int) s_spec_live.empty());
            ++fails;
        } else {
            std::printf("[PASS] G13c drafting ngram: %lld steps drafted, %d tokens in %lld decodes\n",
                        r.summary.drafted_steps, r.summary.n_generated, r.summary.mtp_decodes);
        }
    }

    // G14 — route-ahead: it is lossy by construction, so there is no reference output to compare
    // against. Two things can still be asserted, and together they are what a gate is for.
    //
    // First, the passthrough path. Layers before the horizon route normally, so with the horizon set
    // to the full layer count NOTHING can be overridden and the run must be byte-identical to the
    // unmodified stream. That covers the plumbing: the hook is attached, the gate matrices are
    // mirrored, the watchdog runs, and none of it perturbs a routing it declined to replace.
    RunConfig ra_all_passthrough = base(model);
    ra_all_passthrough.moe.enabled = true;
    ra_all_passthrough.moe.cache_mb = 0;
    ra_all_passthrough.moe.io_threads = 4;
    ra_all_passthrough.moe.route_ahead = MoeStreamConfig::route_ahead_max;
    std::string s_ra_pass;
    if (!gen(ra_all_passthrough, s_ra_pass, err)) {
        std::fprintf(stderr, "route-ahead passthrough run failed: %s\n", err.c_str());
        return 2;
    }
    fails += check("G14a route-ahead(horizon past every layer) == streaming (all passthrough)", s_s0, s_ra_pass);

    // Second, the committed path survives and actually commits. A horizon of one on a cached run
    // must override real routings and still produce output: the committed ids are handed to the
    // speculative read path and adopted by the demand load, so a mistake there reads an expert slot
    // the cache never filled. Asserting the override COUNT is what stops an inert policy passing
    // this vacuously, the same trap G10b guards for the prefetch.
    RunConfig ra_live = base(model);
    ra_live.moe.enabled = true;
    ra_live.moe.cache_mb = 2;
    ra_live.moe.force_cache = true;
    ra_live.moe.io_threads = 4;
    ra_live.moe.route_ahead = 1;
    std::string s_ra_live;
    if (!gen(ra_live, s_ra_live, err)) {
        std::fprintf(stderr, "route-ahead(1) run failed: %s\n", err.c_str());
        return 2;
    }
    {
        RunResult r = run(ra_live);
        if (!r || s_ra_live.empty() || r.summary.route_ahead_overridden <= 0) {
            std::printf("[FAIL] G14b route-ahead(1) must commit routings and generate (committed=%lld, empty=%d)\n",
                        r ? r.summary.route_ahead_overridden : -1, (int) s_ra_live.empty());
            ++fails;
        } else {
            std::printf("[PASS] G14b route-ahead(1) committed %lld routings (%lld passed through) and generated\n",
                        r.summary.route_ahead_overridden, r.summary.route_ahead_passthrough);
        }
    }

    if (fails == 0) std::printf("\nall MoE byte-identity gates passed\n");
    return fails == 0 ? 0 : 1;
}
