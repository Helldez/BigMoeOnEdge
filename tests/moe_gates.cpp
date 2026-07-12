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
//   G4  overlap (async reads + per-expert wait hook) == serial streaming, cache off
//         a) overlap, cache off              b) overlap, small forced cache
//         c) overlap, cache off, io_threads=1 (single lane → maximal stalls)
//
// If G3 passes, the streamer provably never gathers an unrouted (garbage) slice. If G4
// passes, the async path gates each expert correctly — compute never races ahead of its read.
// G4 is compiled only when the fork's expert-ready hook is present (BMOE_HAVE_EXPERT_READY_HOOK).
//
//   S1  two sequential Session generates (warm cache) == one-shot resident, per prompt
//   S2  same, under overlap
//
// S1/S2 guard the session refactor: the expert LRU cache now survives across generate() calls,
// so a second prompt starts warm. That must change only latency, never the produced bytes.
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

    std::string s_res, s_s0, s_sc, s_all, err;
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

    int fails = 0;
    fails += check("G1 resident == streaming(cache off)", s_res, s_s0);
    fails += check("G2 streaming(cache off) == streaming(LRU cache)", s_s0, s_sc);
    fails += check("G3 streaming(selective) == streaming(load-all)", s_s0, s_all);

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

    std::string s_ov0, s_ovc, s_ov1;
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
    fails += check("G4a overlap(cache off) == streaming(cache off)", s_s0, s_ov0);
    fails += check("G4b overlap(LRU cache) == streaming(cache off)", s_s0, s_ovc);
    fails += check("G4c overlap(io_threads=1) == streaming(cache off)", s_s0, s_ov1);
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

    if (fails == 0) std::printf("\nall MoE byte-identity gates passed\n");
    return fails == 0 ? 0 : 1;
}
