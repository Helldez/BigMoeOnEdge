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
#include "bmoe/config.h"
#include "bmoe/runtime.h"

#include <cstdio>
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

    if (fails == 0) std::printf("\nall MoE byte-identity gates passed\n");
    return fails == 0 ? 0 : 1;
}
