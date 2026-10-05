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
//   G18 decide(): a restored prefix state scores == a fresh session computing it (a), a state stored
//       after a partial restore scores == itself as computed (b), == perplexity's choices after the
//       same text (c), resident == streaming (d), a generate() after decide() == one without (e), and
//       a session without decide enabled refuses it harmlessly (f), and with a prefill device
//       decisions (no prefix state kept) and a generate() after them == all CPU (g). G16 and G17 are
//       the prefill-device gates.
//   G19 seeding a saved conversation: a history handed in == the same conversation held live (a), a
//       re-sent history is not prefilled again (b), fit_ctx drops whole exchanges and never a system
//       message (c), and a refused history leaves the session usable (d).
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
//
// The forward predictors (the stale half of G9b, G10b, G14b) target the layer after the one they
// stand in, so they only have something to predict when two MoE blocks are adjacent. A hybrid
// stack that interleaves every MoE block with a Mamba or attention block (nemotron_h_moe) has no
// such pair: there those three are reported N/A instead of failing, while their identity halves
// and the G9b control still run and must pass.
#include "bmoe/config.h"
#include "bmoe/runtime.h"
#include "bmoe/session.h"
#include "gguf_offsets.h"

#include "ggml-backend.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace bmoe;

// ── G16 support: a second device on any host ────────────────────────────────────────────────────
// The prefill-device path moves weights onto another backend's buffers between graphs. The RPC
// backend fronting this same CPU is a device every host has, and since it computes with the very
// kernels the local CPU uses, placement is the ONLY thing that differs: the output must be
// byte-identical. Hexagon's own numerics (fp16 on HMX) are a device question, not this gate's.

static const char * const kRpcEndpoint = "127.0.0.1:50599";

// True once something accepts on the endpoint. The RPC client aborts the process when it cannot
// connect, so the server has to be seen listening before it is registered.
static bool port_accepts(int port) {
#if defined(_WIN32)
    static bool wsa = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    if (!wsa) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short) port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    const bool ok = connect(s, (sockaddr *) &a, sizeof(a)) == 0;
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
    return ok;
}

// Starts an in-process rpc-server over the CPU device and returns the client device's registry
// name, or "" when this build has no RPC backend.
static std::string loopback_rpc_device() {
    ggml_backend_reg_t rpc = ggml_backend_reg_by_name("RPC");
    ggml_backend_dev_t cpu = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
    if (!rpc || !cpu) return "";
    using start_fn = void (*)(const char *, const char *, size_t, size_t, ggml_backend_dev_t *);
    using add_fn = ggml_backend_reg_t (*)(const char *);
    auto start = (start_fn) ggml_backend_reg_get_proc_address(rpc, "ggml_backend_rpc_start_server");
    auto add = (add_fn) ggml_backend_reg_get_proc_address(rpc, "ggml_backend_rpc_add_server");
    if (!start || !add) return "";
    // Serves until the process exits; the gates are one process.
    std::thread([start, cpu] {
        ggml_backend_dev_t devs[1] = {cpu};
        start(kRpcEndpoint, nullptr, 2, 1, devs);
    }).detach();
    for (int i = 0; i < 200 && !port_accepts(50599); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (!port_accepts(50599)) return "";
    ggml_backend_reg_t srv = add(kRpcEndpoint);
    if (!srv || ggml_backend_reg_dev_count(srv) == 0) return "";
    ggml_backend_register(srv);
    return ggml_backend_dev_name(ggml_backend_reg_dev_get(srv, 0));
}

// Whether the model's trunk has two adjacent MoE blocks, read from the file: a MoE block is one
// with a router (blk.<il>.ffn_gate_inp), and a NextN/MTP block (it carries nextn.* tensors) is not
// part of the trunk the predictors walk. A file it cannot read counts as adjacent, so a parse
// failure keeps every check strict rather than excusing it.
static bool has_adjacent_moe_layers(const std::string & model) {
    const GgufOffsets off = read_gguf_offsets(model.c_str());
    if (!off.ok) return true;
    auto has = [&](int il, const char * suffix) {
        return off.off_by_name.count("blk." + std::to_string(il) + "." + suffix) != 0;
    };
    auto is_trunk_moe = [&](int il) { return has(il, "ffn_gate_inp.weight") && !has(il, "nextn.eh_proj.weight"); };
    for (const auto & kv : off.off_by_name) {
        int il = -1;
        if (std::sscanf(kv.first.c_str(), "blk.%d.", &il) == 1 && is_trunk_moe(il) && is_trunk_moe(il + 1)) return true;
    }
    return false;
}

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
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not swallow the verdicts printed before it
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <tiny-moe.gguf>\n", argv[0]);
        return 2;
    }
    const std::string model = argv[1];
    const bool adjacent_moe = has_adjacent_moe_layers(model);

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
        if (!r || self.rows == 0 || (adjacent_moe && stale.rows == 0) || prev.rows == 0) {
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
        if (r && !adjacent_moe) {
            std::printf("[N/A ] G10b predict-prefetch: no two adjacent MoE blocks, nothing to predict forward\n");
        } else if (!r || r.summary.moe_spec_experts <= 0) {
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
        if (r && !s_ra_live.empty() && !adjacent_moe) {
            std::printf("[N/A ] G14b route-ahead(1): no two adjacent MoE blocks, nothing to commit ahead\n");
        } else if (!r || s_ra_live.empty() || r.summary.route_ahead_overridden <= 0) {
            std::printf("[FAIL] G14b route-ahead(1) must commit routings and generate (committed=%lld, empty=%d)\n",
                        r ? r.summary.route_ahead_overridden : -1, (int) s_ra_live.empty());
            ++fails;
        } else {
            std::printf("[PASS] G14b route-ahead(1) committed %lld routings (%lld passed through) and generated\n",
                        r.summary.route_ahead_overridden, r.summary.route_ahead_passthrough);
        }
    }

    // ── G16: prefill on a device, decode on the CPU ──
    // The reference prefills in the same 4-token ubatches, all on the CPU, so the graphs are the same
    // shapes and the only difference is where the wide ones ran. a) one generate: identical, and some
    // prompt tokens must actually have gone to the device, or the gate passes vacuously (the trap G10b
    // and G14b guard). b) two generates in one session: the weights go back to the host for decode and
    // out again for the next prompt, which is where a stale reused graph would read a device buffer
    // from the CPU. c) perplexity, which compares every position's logits instead of one greedy path.
    {
        const std::string dev = loopback_rpc_device();
        if (dev.empty()) {
            std::printf("[SKIP] G16 prefill device: this build has no RPC backend (GGML_RPC=OFF)\n");
        } else {
            RunConfig pref = base(model);
            pref.moe.enabled = false;
            pref.n_ubatch = 4;
            RunConfig pdev = pref;
            pdev.prefill.device = dev;
            pdev.prefill.min_tokens = 4;

            std::string s_pref;
            if (!gen(pref, s_pref, err)) {
                std::fprintf(stderr, "G16 reference run failed: %s\n", err.c_str());
                return 2;
            }
            RunResult r = run(pdev);
            if (!r) {
                std::fprintf(stderr, "G16 prefill-device run failed: %s\n", r.error.c_str());
                return 2;
            }
            fails += check("G16a prefill on device, decode on CPU == all CPU", s_pref, r.generated_text);
            // Tokens say the weights were moved; nodes say the device computed. Both, or the in-process
            // server (whose buffers the CPU could read directly) would let a silent fallback pass.
            if (r.summary.prefill_device_tokens <= 0 || r.summary.prefill_device_nodes <= 0) {
                std::printf("[FAIL] G16a the device did not prefill (%d of %d tokens, %lld nodes)\n",
                            r.summary.prefill_device_tokens, r.summary.n_prompt, r.summary.prefill_device_nodes);
                ++fails;
            } else {
                std::printf("[PASS] G16a the device prefilled %d of %d prompt tokens (%lld nodes)\n",
                            r.summary.prefill_device_tokens, r.summary.n_prompt, r.summary.prefill_device_nodes);
            }

            std::string open_err;
            std::unique_ptr<Session> s = Session::open(session_config_from(pdev), open_err);
            if (!s) {
                std::fprintf(stderr, "G16 session open failed: %s\n", open_err.c_str());
                return 2;
            }
            GenerateRequest req;
            req.prompt = pdev.prompt;
            req.n_predict = pdev.n_predict;
            req.clear_kv = true;
            RunResult g1 = s->generate(req);
            RunResult g2 = s->generate(req);
            if (!g1 || !g2) {
                std::fprintf(stderr, "G16 session generate failed: %s\n", (!g1 ? g1.error : g2.error).c_str());
                return 2;
            }
            fails += check("G16b session generate #1 (device prefill) == all CPU", s_pref, g1.generated_text);
            fails += check("G16b session generate #2 (device prefill again) == all CPU", s_pref, g2.generated_text);

            PplRequest pr;
            pr.text = "The quick brown fox jumps over the lazy dog, and then it runs back home to sleep.";
            pr.skip = 2;
            std::unique_ptr<Session> sc = Session::open(session_config_from(pref), open_err);
            if (!sc) {
                std::fprintf(stderr, "G16 reference session open failed: %s\n", open_err.c_str());
                return 2;
            }
            PplResult p_cpu = sc->perplexity(pr);
            PplResult p_dev = s->perplexity(pr);
            if (!p_cpu.ok || !p_dev.ok) {
                std::fprintf(stderr, "G16 perplexity failed: %s\n", (!p_cpu.ok ? p_cpu.error : p_dev.error).c_str());
                return 2;
            }
            char a[64], b[64];
            std::snprintf(a, sizeof(a), "%.17g/%d", p_cpu.nll, p_cpu.n_top1);
            std::snprintf(b, sizeof(b), "%.17g/%d", p_dev.nll, p_dev.n_top1);
            fails += check("G16c perplexity with device prefill == all CPU (nll/top1, bit for bit)", a, b);

            // ── G17: streamed experts through the device arena ──
            // The model streams; the device prefill gets its experts from two slots refilled layer by
            // layer while decode keeps the streamer's cache. Same reference, same pieces. Variants: no
            // cache, a small evicting cache (decode must not see a trace of the arena), and one loader
            // (every layer waits at its barrier, the ordering the slots rely on at its tightest). Each
            // must also show that the arena read experts and the device computed.
            struct Variant {
                const char * name;
                int cache_mb;
                int loaders; // the arena's loader threads (--prefill-loaders), not the decode lanes
                int delay_us;
                bool routed = false;   // read only routed experts (PrefillDeviceConfig::routed)
                bool sabotage = false; // routed, with the routing-node reads skipped: must be caught
            };
            const Variant variants[] = {
                {"G17a arena, cache off, 4 loaders", 0, 4, 0},
                {"G17b arena, small forced cache, 4 loaders", 2, 4, 0},
                // Loads slowed until the graph always reaches a layer first: only the barrier orders them.
                {"G17c arena, cache off, 1 slowed loader", 0, 1, 20000},
                // Routed: every layer after the first graph is read from a prediction plus what the
                // routing adds. The fallback to whole layers is disabled so the tiny model exercises it.
                {"G17f arena routed, 4 loaders", 0, 4, 0, true},
                {"G17f arena routed, 1 slowed loader", 0, 1, 20000, true},
                {"G17g arena routed with its routing-node reads sabotaged", 0, 4, 0, true, true},
            };
            // Greedy text alone is a weak witness here: on the tiny model a slot holding the wrong
            // layer's experts can still produce the same few tokens (measured, with the barrier
            // sabotaged). So each variant is also scored for perplexity, bit for bit, and must read the
            // same bytes as the first: a graph that outran its loads reads less, since the drained
            // queue is dropped.
            double arena_mib_ref = -1.0;
            for (const Variant & v : variants) {
                RunConfig c = pdev;
                c.moe.enabled = true;
                c.moe.cache_mb = v.cache_mb;
                c.moe.force_cache = v.cache_mb > 0;
                c.prefill.load_threads = v.loaders;
                c.prefill.test_load_delay_us = v.delay_us;
                c.prefill.routed = v.routed;
                c.prefill.routed_full_frac = 1.0f;
                c.prefill.test_routed_skip_demand = v.sabotage;
                std::unique_ptr<Session> vs = Session::open(session_config_from(c), open_err);
                if (!vs) {
                    std::fprintf(stderr, "%s open failed: %s\n", v.name, open_err.c_str());
                    return 2;
                }
                RunResult rr = vs->generate(req);
                if (!rr) {
                    std::fprintf(stderr, "%s failed: %s\n", v.name, rr.error.c_str());
                    return 2;
                }
                if (v.sabotage) {
                    // Only the perplexity pass is a reliable witness (see above); it runs after the
                    // generate, so its graphs are all predicted ones.
                    PplResult pv = vs->perplexity(pr);
                    char cv[64];
                    std::snprintf(cv, sizeof(cv), "%.17g/%d", pv.nll, pv.n_top1);
                    if (pv.ok && std::string(cv) == a) {
                        std::printf("[FAIL] %s: the result did not change (%s); the routed gate proves nothing\n",
                                    v.name, cv);
                        ++fails;
                    } else {
                        std::printf("[PASS] %s: caught (%s vs %s)\n", v.name, pv.ok ? cv : "failed", a);
                    }
                    continue;
                }
                fails += check(v.name, s_pref, rr.generated_text);
                const double mib = rr.summary.prefill_device_read_mib;
                if (arena_mib_ref < 0.0) arena_mib_ref = mib;
                const bool mib_ok = v.routed ? (mib > 0.0 && mib < arena_mib_ref) : mib == arena_mib_ref;
                if (mib <= 0.0 || !mib_ok || rr.summary.prefill_device_nodes <= 0) {
                    std::printf("[FAIL] %s: the arena read %.3f MiB (expected %.3f), the device computed %lld nodes\n",
                                v.name, mib, arena_mib_ref, rr.summary.prefill_device_nodes);
                    ++fails;
                } else {
                    std::printf("[PASS] %s: the arena read %.3f MiB, the device computed %lld nodes\n", v.name, mib,
                                rr.summary.prefill_device_nodes);
                }
                PplResult pv = vs->perplexity(pr);
                if (!pv.ok) {
                    std::fprintf(stderr, "%s perplexity failed: %s\n", v.name, pv.error.c_str());
                    return 2;
                }
                char cv[64];
                std::snprintf(cv, sizeof(cv), "%.17g/%d", pv.nll, pv.n_top1);
                fails += check((std::string(v.name) + ": perplexity bit for bit").c_str(), a, cv);
            }

            // Two generates and a perplexity pass through one streamed session: the arena refills per
            // graph and the streamer's cache carries on underneath it.
            RunConfig cs = pdev;
            cs.moe.enabled = true;
            cs.moe.cache_mb = 2;
            cs.moe.force_cache = true;
            cs.moe.io_threads = 4;
            std::unique_ptr<Session> ss = Session::open(session_config_from(cs), open_err);
            if (!ss) {
                std::fprintf(stderr, "G17 session open failed: %s\n", open_err.c_str());
                return 2;
            }
            RunResult h1 = ss->generate(req);
            RunResult h2 = ss->generate(req);
            if (!h1 || !h2) {
                std::fprintf(stderr, "G17 session generate failed: %s\n", (!h1 ? h1.error : h2.error).c_str());
                return 2;
            }
            fails += check("G17d streamed session generate #1 == all CPU", s_pref, h1.generated_text);
            fails += check("G17d streamed session generate #2 == all CPU", s_pref, h2.generated_text);
            PplResult p_arena = ss->perplexity(pr);
            if (!p_arena.ok) {
                std::fprintf(stderr, "G17 perplexity failed: %s\n", p_arena.error.c_str());
                return 2;
            }
            char c3[64];
            std::snprintf(c3, sizeof(c3), "%.17g/%d", p_arena.nll, p_arena.n_top1);
            fails += check("G17e perplexity through the arena == all CPU (nll/top1, bit for bit)", a, c3);
        }
    }

    // G18 — decide(): a choice read from one prefill, with the prefix state kept between calls.
    //
    // a) the kept state is the computed state: a call that restores its prefix scores exactly what
    //    a fresh session computing the same prompt scores (same split, so the same prefill pieces).
    //    The restore count is asserted too, or a cache that never hit would pass vacuously.
    // b) when the next prefix extends the kept one (an agent's history growing), the state stored
    //    after the partial restore answers, restored, exactly as it did when computed.
    // c) decide and perplexity read the same distribution: with the prefix state off (one whole
    //    prefill) the choices score exactly as PplRequest::choices scores them after the same text.
    // d) streamed == resident, cold and warm, with and without an evicting cache.
    {
        const std::string prefix = "Pick the next step. Task: open the settings. History: none. ";
        const std::string grown_prefix = prefix + "Step 1: tapped B. ";
        auto decide_req = [](const std::string & p, const std::string & s, bool reuse) {
            DecideRequest q;
            q.prefix = p;
            q.suffix = s;
            q.choices = {"A", "B", "C"};
            q.reuse_prefix = reuse;
            return q;
        };
        const DecideRequest q1 = decide_req(prefix, "Screen: A) Wi-Fi B) Battery C) Display. Answer:", true);
        const DecideRequest q2 = decide_req(prefix, "Screen: A) Sound B) Storage C) About. Answer:", true);
        const DecideRequest q3 = decide_req(grown_prefix, "Screen: A) Back B) Home C) Search. Answer:", true);

        // Open a session on `c` and run `qs` in order. False (with err) if any call fails.
        auto decide_seq = [&](const RunConfig & c, const std::vector<DecideRequest> & qs,
                              std::vector<DecideResult> & out) {
            SessionConfig sc = session_config_from(c);
            sc.decide.enabled = true;
            sc.decide.prefix_cache = PrefixCacheMode::On;
            std::unique_ptr<Session> s = Session::open(sc, err);
            if (!s) return false;
            out.clear();
            for (const DecideRequest & q : qs) {
                out.push_back(s->decide(q));
                if (!out.back().ok) {
                    err = out.back().error;
                    return false;
                }
            }
            return true;
        };
        auto logps = [](const DecideResult & r) {
            std::string s;
            char buf[64];
            for (double v : r.choice_logp) {
                std::snprintf(buf, sizeof buf, "%a ", v); // hex float: exact, so equal strings mean equal bits
                s += buf;
            }
            return s;
        };

        std::vector<DecideResult> warm, cold2, grown;
        if (!decide_seq(stream0, {q1, q2}, warm) || !decide_seq(stream0, {q2}, cold2) ||
            !decide_seq(stream0, {q1, q3, q3}, grown)) {
            std::fprintf(stderr, "decide run failed: %s\n", err.c_str());
            return 2;
        }
        if (warm[1].n_reused <= 0) {
            std::printf("[FAIL] G18a decide must restore its prefix (reused %d tokens)\n", warm[1].n_reused);
            ++fails;
        }
        fails += check("G18a decide(prefix restored) == decide(fresh session)", logps(cold2[0]), logps(warm[1]));
        // The grown prefix is compared with itself, not with a fresh session: a fresh session prefills
        // [0, grown) in one piece where the growing one prefilled [0, prefix) and then the growth, and
        // llama.cpp does not round the same across different piece boundaries (the last bit of the
        // log-probs moves, as it does for any change of prefill chunking). What must hold exactly is
        // that the state stored after a partial restore is the state that answered.
        if (grown[1].n_reused <= 0 || grown[2].n_reused <= grown[1].n_reused) {
            std::printf("[FAIL] G18b decide must restore the old prefix, then the grown one (reused %d, then %d)\n",
                        grown[1].n_reused, grown[2].n_reused);
            ++fails;
        }
        fails += check("G18b decide(grown prefix restored) == decide(grown prefix as computed)", logps(grown[1]),
                       logps(grown[2]));

        std::vector<DecideResult> whole;
        DecideRequest q_whole = q1;
        q_whole.reuse_prefix = false;
        if (!decide_seq(stream0, {q_whole}, whole)) {
            std::fprintf(stderr, "decide(whole) run failed: %s\n", err.c_str());
            return 2;
        }
        {
            SessionConfig sc = session_config_from(stream0);
            std::unique_ptr<Session> s = Session::open(sc, err);
            if (!s) {
                std::fprintf(stderr, "perplexity session failed: %s\n", err.c_str());
                return 2;
            }
            PplRequest pq;
            pq.text = q1.prefix + q1.suffix;
            pq.choices = q1.choices;
            PplResult pr = s->perplexity(pq);
            if (!pr.ok) {
                std::fprintf(stderr, "perplexity run failed: %s\n", pr.error.c_str());
                return 2;
            }
            DecideResult as_decide;
            as_decide.choice_logp = pr.choice_logp;
            fails += check("G18c decide(whole prefill) == perplexity(choices)", logps(as_decide), logps(whole[0]));

            // f) off by default: the same session (decide not enabled) refuses, without harm.
            const DecideResult off = s->decide(q1);
            const RunResult after = s->generate(GenerateRequest{stream0.prompt, stream0.n_predict});
            if (off.ok || off.fatal || !after) {
                std::printf("[FAIL] G18f decide off must refuse, not fatally (ok=%d fatal=%d, generate after: %s)\n",
                            (int) off.ok, (int) off.fatal, after ? "ok" : after.error.c_str());
                ++fails;
            } else {
                fails +=
                    check("G18f decide off: refused, and generate after it == generate", s_s0, after.generated_text);
            }
        }

        std::vector<DecideResult> res_seq, cached_seq;
        if (!decide_seq(resident, {q1, q2, q3}, res_seq) || !decide_seq(streamc, {q1, q2, q3}, cached_seq)) {
            std::fprintf(stderr, "decide run failed: %s\n", err.c_str());
            return 2;
        }
        std::vector<DecideResult> s0_seq;
        if (!decide_seq(stream0, {q1, q2, q3}, s0_seq)) {
            std::fprintf(stderr, "decide run failed: %s\n", err.c_str());
            return 2;
        }
        std::string a, b, c;
        for (size_t i = 0; i < res_seq.size(); ++i) {
            a += logps(res_seq[i]) + "| ";
            b += logps(s0_seq[i]) + "| ";
            c += logps(cached_seq[i]) + "| ";
        }
        fails += check("G18d decide: resident == streaming(cache off)", a, b);
        fails += check("G18d decide: resident == streaming(LRU cache)", a, c);

        // e) decide leaves nothing behind: after a decision and a refused one, a generate() that asks
        //    to CONTINUE the conversation (clear_kv=false) must still start from an empty sequence.
        {
            SessionConfig sc = session_config_from(stream0);
            sc.decide.enabled = true;
            std::unique_ptr<Session> s = Session::open(sc, err);
            if (!s) {
                std::fprintf(stderr, "decide+generate session failed: %s\n", err.c_str());
                return 2;
            }
            DecideRequest refused = q1;
            refused.choices = {"Yes", "Yeah"};
            const DecideResult d_ok = s->decide(q1);
            const DecideResult d_refused = s->decide(refused);
            GenerateRequest g;
            g.prompt = stream0.prompt;
            g.n_predict = stream0.n_predict;
            g.clear_kv = false;
            const RunResult gr = s->generate(g);
            if (!d_ok.ok || d_refused.ok || d_refused.fatal || !gr) {
                std::printf("[FAIL] G18e decide then generate (decide ok=%d, refused ok=%d fatal=%d, generate: %s)\n",
                            (int) d_ok.ok, (int) d_refused.ok, (int) d_refused.fatal, gr ? "ok" : gr.error.c_str());
                ++fails;
            } else {
                fails += check("G18e generate after decide == generate", s_s0, gr.generated_text);
            }
        }

        // g) decide with a prefill device (G16's loopback device and 4-token pieces), prefix cache on
        //    auto as the app runs it: auto keeps no state with a device (llama.cpp would save it from
        //    where the model state was before the move), so nothing may be restored, and three
        //    decisions and a generate() after them must equal the same session all on the CPU with
        //    the cache off. The generate fails if a decision left the weights on the device or the
        //    moved state uncleared. Some decision tokens must have gone to the device, or the gate
        //    passes vacuously.
        {
            const std::string dev = loopback_rpc_device();
            if (dev.empty()) {
                std::printf("[SKIP] G18g decide on a prefill device: this build has no RPC backend (GGML_RPC=OFF)\n");
            } else {
                RunConfig pref = base(model);
                pref.moe.enabled = false;
                pref.n_ubatch = 4;
                RunConfig pdev = pref;
                pdev.prefill.device = dev;
                pdev.prefill.min_tokens = 4;
                auto decide_then_generate = [&](const RunConfig & c, PrefixCacheMode mode, std::string & out,
                                                int & dev_tokens, int & reused) {
                    SessionConfig sc = session_config_from(c);
                    sc.decide.enabled = true;
                    sc.decide.prefix_cache = mode;
                    std::unique_ptr<Session> s = Session::open(sc, err);
                    if (!s) return false;
                    out.clear();
                    dev_tokens = 0;
                    reused = 0;
                    for (const DecideRequest & q : {q1, q2, q3}) {
                        const DecideResult d = s->decide(q);
                        if (!d.ok) {
                            err = d.error;
                            return false;
                        }
                        dev_tokens += d.prefill.device_tokens;
                        reused += d.n_reused;
                        out += logps(d) + "| ";
                    }
                    GenerateRequest g;
                    g.prompt = c.prompt;
                    g.n_predict = c.n_predict;
                    g.clear_kv = false;
                    const RunResult gr = s->generate(g);
                    if (!gr) {
                        err = gr.error;
                        return false;
                    }
                    out += gr.generated_text;
                    return true;
                };
                std::string on_cpu, on_dev;
                int cpu_dev_tokens = 0, cpu_reused = 0, dev_tokens = 0, dev_reused = 0;
                if (!decide_then_generate(pref, PrefixCacheMode::Off, on_cpu, cpu_dev_tokens, cpu_reused) ||
                    !decide_then_generate(pdev, PrefixCacheMode::Auto, on_dev, dev_tokens, dev_reused)) {
                    std::fprintf(stderr, "G18g decide run failed: %s\n", err.c_str());
                    return 2;
                }
                if (dev_tokens <= 0 || dev_reused != 0) {
                    std::printf("[FAIL] G18g device decisions: %d tokens on the device (want > 0), %d restored "
                                "(want 0)\n",
                                dev_tokens, dev_reused);
                    ++fails;
                }
                fails += check("G18g decide + generate on a prefill device == all CPU", on_cpu, on_dev);
            }
        }
    }

    // G19 — seeding a saved conversation (GenerateRequest::history, fit_ctx). A front-end whose own
    // store is the source of truth re-sends the whole conversation each turn; the engine must treat
    // that as if it had held it all along.
    //
    // a) seeded == live: the answer to turn two, and the context length after it, are the same
    //    whether the engine decoded turn one itself or was handed it as history (and so prefilled it).
    // b) prefix reuse: re-sending the conversation the session already holds prefills only the new
    //    turn, not the history again.
    // c) fit_ctx drops the oldest exchanges and says how many; without it the same request is
    //    refused, and a system message is never what gets dropped.
    // d) a refused request is harmless: non-chat sessions and unknown roles are non-fatal errors, and
    //    the session answers the next request.
    {
        RunConfig cc = base(model);
        cc.moe.enabled = false;
        cc.chatml = true;
        cc.n_ctx = 512;
        cc.n_predict = 12;
        const std::string u1 = "Hello there.", u2 = "And then?", u3 = "ok?";

        auto turn = [&](Session & s, const std::string & prompt, bool clear, const std::vector<ChatMessage> * hist,
                        bool fit = false) {
            GenerateRequest g;
            g.prompt = prompt;
            g.n_predict = cc.n_predict;
            g.clear_kv = clear;
            if (hist) {
                g.replace_history = true;
                g.history = *hist;
            }
            g.fit_ctx = fit;
            return s.generate(g);
        };

        std::unique_ptr<Session> s = Session::open(session_config_from(cc), err);
        if (!s) {
            std::fprintf(stderr, "G19 session open failed: %s\n", err.c_str());
            return 2;
        }
        const RunResult t1 = turn(*s, u1, true, nullptr);
        const RunResult t2 = turn(*s, u2, false, nullptr);
        if (!t1 || !t2) {
            std::fprintf(stderr, "G19 live turns failed: %s\n", (!t1 ? t1.error : t2.error).c_str());
            return 2;
        }
        const std::vector<ChatMessage> h1 = {{"user", u1}, {"assistant", t1.generated_text}};
        const RunResult t2s = turn(*s, u2, true, &h1);
        if (!t2s) {
            std::fprintf(stderr, "G19 seeded turn failed: %s\n", t2s.error.c_str());
            return 2;
        }
        fails += check("G19a seeded answer == live answer", t2.generated_text, t2s.generated_text);
        fails += check("G19a seeded n_past == live n_past", std::to_string(t2.summary.n_past),
                       std::to_string(t2s.summary.n_past));

        const std::vector<ChatMessage> h2 = {
            {"user", u1}, {"assistant", t1.generated_text}, {"user", u2}, {"assistant", t2s.generated_text}};
        const RunResult t3 = turn(*s, u3, false, &h2);
        if (!t3) {
            std::fprintf(stderr, "G19 reuse turn failed: %s\n", t3.error.c_str());
            return 2;
        }
        // What "reused" means is what the session would have prefilled had it held the conversation
        // all along, so the reference is a session that did. A fixed bound on the new turn's size does
        // not do: on this byte-level vocab the template's own markers are dozens of tokens, and the
        // tail of the last answer is re-rendered whatever happens. The cold figure keeps the equality
        // from passing when nothing was reused at all.
        std::unique_ptr<Session> held = Session::open(session_config_from(cc), err);
        if (!held) {
            std::fprintf(stderr, "G19 live session open failed: %s\n", err.c_str());
            return 2;
        }
        turn(*held, u1, true, nullptr);
        turn(*held, u2, false, nullptr);
        const RunResult t3_live = turn(*held, u3, false, nullptr);
        const RunResult t3_cold = turn(*s, u3, true, &h2);
        if (!t3_live || !t3_cold) {
            std::fprintf(stderr, "G19 reference turns failed: %s\n",
                         (!t3_live ? t3_live.error : t3_cold.error).c_str());
            return 2;
        }
        const int n_reused = t3.summary.n_prompt, n_live = t3_live.summary.n_prompt, n_cold = t3_cold.summary.n_prompt;
        // A recurrent state cannot be rewound to the middle of a conversation, so a hybrid stack
        // re-prefills every turn whether or not the history was seeded: the equality still holds and
        // is checked, but there is no saving for the cold figure to show.
        const bool rewindable = s->arch() != "nemotron_h_moe";
        if (n_reused != n_live || (rewindable && n_reused * 2 >= n_cold)) {
            std::printf("[FAIL] G19b re-sent history prefilled %d tokens (held live: %d, cold: %d)\n", n_reused, n_live,
                        n_cold);
            ++fails;
        } else {
            std::printf("[PASS] G19b re-sent history prefilled %d tokens (held live: %d, cold: %d)%s\n", n_reused,
                        n_live, n_cold, rewindable ? "" : "; no prefix reuse on a recurrent stack");
        }

        // c) a window too small for the history. 40 bytes of text is roughly 40 tokens on this
        //    byte-level vocab, so four exchanges cannot share 256 positions with a system message.
        RunConfig small = cc;
        small.n_ctx = 256;
        std::unique_ptr<Session> sm = Session::open(session_config_from(small), err);
        if (!sm) {
            std::fprintf(stderr, "G19 small session open failed: %s\n", err.c_str());
            return 2;
        }
        const std::string pad(40, 'x');
        std::vector<ChatMessage> big = {{"system", "Be brief."}};
        for (int i = 0; i < 4; ++i) {
            big.push_back({"user", pad});
            big.push_back({"assistant", pad});
        }
        const RunResult unfit = turn(*sm, u3, true, &big, /*fit*/ false);
        const RunResult fitted = turn(*sm, u3, true, &big, /*fit*/ true);
        const bool refused = !unfit.ok && unfit.error.find("exceeds the session n_ctx") != std::string::npos;
        if (!refused) {
            std::printf("[FAIL] G19c without fit_ctx an oversized history is refused (%s)\n",
                        unfit.ok ? "it ran" : unfit.error.c_str());
            ++fails;
        } else {
            std::printf("[PASS] G19c without fit_ctx an oversized history is refused\n");
        }
        // Whole exchanges only: an even count of messages, at least one, and fewer than all eight (the
        // system message is not one of them, so it cannot be what makes the count reach nine).
        if (!fitted.ok || fitted.history_dropped <= 0 || fitted.history_dropped % 2 != 0 ||
            fitted.history_dropped > (int) big.size() - 1) {
            std::printf("[FAIL] G19c fit_ctx drops whole exchanges (ok=%d dropped=%d: %s)\n", (int) fitted.ok,
                        fitted.history_dropped, fitted.ok ? "" : fitted.error.c_str());
            ++fails;
        } else {
            std::printf("[PASS] G19c fit_ctx drops whole exchanges (dropped %d of %d messages)\n",
                        fitted.history_dropped, (int) big.size() - 1);
        }
        // A system message that is itself too big has nothing droppable behind it: still refused.
        const std::vector<ChatMessage> huge_sys = {
            {"system", std::string(400, 's')}, {"user", pad}, {"assistant", pad}};
        const RunResult sys_only = turn(*sm, u3, true, &huge_sys, /*fit*/ true);
        if (sys_only.ok) {
            std::printf("[FAIL] G19c fit_ctx never drops a system message (the request ran, dropped %d)\n",
                        sys_only.history_dropped);
            ++fails;
        } else {
            std::printf("[PASS] G19c fit_ctx never drops a system message\n");
        }

        // d) refusals leave the session usable.
        const std::vector<ChatMessage> bad_role = {{"tool", "x"}};
        const RunResult r_role = turn(*s, u3, true, &bad_role);
        const RunResult after = turn(*s, u3, true, nullptr);
        RunConfig raw = cc;
        raw.chatml = false;
        std::unique_ptr<Session> sr = Session::open(session_config_from(raw), err);
        if (!sr) {
            std::fprintf(stderr, "G19 raw session open failed: %s\n", err.c_str());
            return 2;
        }
        const RunResult r_raw = turn(*sr, u3, true, &h1);
        const bool d_ok = !r_role.ok && r_role.error.find("unknown history role") != std::string::npos && after.ok &&
                          !r_raw.ok && r_raw.error.find("history needs chat mode") != std::string::npos;
        if (!d_ok) {
            std::printf("[FAIL] G19d refused histories (role: %s; next turn ok=%d; raw: %s)\n",
                        r_role.ok ? "ran" : r_role.error.c_str(), (int) after.ok,
                        r_raw.ok ? "ran" : r_raw.error.c_str());
            ++fails;
        } else {
            std::printf("[PASS] G19d refused histories are non-fatal errors\n");
        }
    }

    if (fails == 0) std::printf("\nall MoE byte-identity gates passed\n");
    return fails == 0 ? 0 : 1;
}
