// bmoe-iobench — how far does flash read bandwidth scale with concurrent lanes?
//
// The streamer's "queue depth" is exactly its lane count: every lane issues one blocking pread
// at a time, because the async submission APIs (io_uring, kernel AIO) are not reachable from an
// Android app. So the only way to keep more reads in flight is more lanes, and the engine caps
// that at MoeStreamConfig::io_threads_max. This measures whether that cap sits below the
// device's ceiling — the question that has to be answered BEFORE raising it, since a cap that is
// already at the hardware limit costs memory (one fd + one bounce buffer per lane) for nothing.
//
// It deliberately drives bmoe::FileReader, the same read path the engine uses, rather than a
// hand-rolled pread loop: the alignment, the bounce buffer and the O_DIRECT verification/fallback
// are part of what is being measured. Nothing here links the engine or llama.cpp — the I/O layer
// stands alone, so this builds in seconds and cannot perturb the streamer.
//
// Bandwidth is judged against the ALIGNED WINDOW the drive actually served (FileReader::read's
// return), not the bytes requested — the difference is real device traffic.
//
// `--compute-load N` runs N CPU-burning threads alongside the lanes. Without it the sweep measures
// the drive on an idle CPU, which is not the condition the streamer reads under: the engine reads
// while ggml's compute threads spin. If per-lane bandwidth falls once the CPU is busy, the lanes are
// starved of scheduler time and the bottleneck is contention, not the flash.
//
//   bmoe-iobench --model M.gguf [--lanes 1,2,4,8,16] [--slice-kb 4096] [--seconds 5] [--buffered]
//                [--compute-load N]
#include "file_reader.h"
#include "platform_io.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using clock_t_ = std::chrono::steady_clock;

// Deterministic per-lane offset stream. A real RNG would make two lane counts read different
// regions and turn a bandwidth comparison into a luck comparison; this way lane L always walks
// the same sequence whatever the lane count, so the only variable between rows is concurrency.
struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
    uint64_t next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return s >> 16;
    }
};

struct LaneResult {
    long long window_bytes = 0; // what the drive served (aligned)
    long long reads = 0;
    long long busy_ns = 0;
};

// One lane: random-offset reads of `slice` bytes until the deadline. Offsets are block-aligned and
// kept a slice away from EOF so every read is a full window (the sub-alignment tail would otherwise
// take FileReader's buffered fallback and quietly measure the page cache instead of the drive).
void lane_worker(bmoe::FileReader * r, int lane, size_t slice, size_t align, uint64_t fsize,
                 clock_t_::time_point deadline, LaneResult * out) {
    void * dst = bmoe::pio::alloc_aligned(align, slice);
    if (!dst) return;
    const uint64_t span = (fsize > slice * 2) ? (fsize - slice * 2) : 0;
    if (span == 0) {
        bmoe::pio::aligned_free(dst);
        return;
    }
    Lcg rng((uint64_t) lane + 1);
    LaneResult acc;
    while (clock_t_::now() < deadline) {
        const uint64_t off = (rng.next() % span) & ~(uint64_t) (align - 1);
        const auto t0 = clock_t_::now();
        const long long got = r->read(lane, dst, off, slice);
        const auto t1 = clock_t_::now();
        if (got < 0) break;
        acc.window_bytes += got;
        acc.reads++;
        acc.busy_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }
    bmoe::pio::aligned_free(dst);
    *out = acc;
}

// A stand-in for the compute threads the reads actually compete with. It only has to keep a core
// busy in userspace; the arithmetic is irrelevant, but it must not be optimised away, hence the
// volatile sink.
void load_worker(clock_t_::time_point deadline, std::atomic<bool> * stop) {
    volatile double sink = 0.0;
    double x = 1.000001;
    while (!stop->load(std::memory_order_relaxed) && clock_t_::now() < deadline) {
        for (int i = 0; i < 4096; ++i) x = x * 1.0000001 + 1e-9;
        sink = sink + x;
        if (x > 1e6) x = 1.000001;
    }
    (void) sink;
}

// One row of the sweep: open a reader with `lanes` lanes, run them all for `seconds`, report the
// aggregate. The reader is opened and closed per row so each row pays its own warm-up and no lane
// inherits another row's fd state.
bool run_row(const std::string & path, int lanes, size_t slice, bool direct, double seconds, int load,
             double * mibs_out) {
    bmoe::FileReader r;
    const size_t align = 4096;
    const size_t bounce_cap = slice + 2 * align; // mirrors what the streamer asks for
    if (!r.open(path, lanes, direct, align, bounce_cap)) {
        std::fprintf(stderr, "open failed (lanes=%d)\n", lanes);
        return false;
    }
    std::vector<LaneResult> res((size_t) lanes);
    std::vector<std::thread> th;
    th.reserve((size_t) lanes);

    const auto t0 = clock_t_::now();
    const auto deadline = t0 + std::chrono::milliseconds((long long) (seconds * 1000.0));
    std::atomic<bool> stop{false};
    std::vector<std::thread> loaders;
    loaders.reserve((size_t) (load > 0 ? load : 0));
    for (int i = 0; i < load; ++i) loaders.emplace_back(load_worker, deadline, &stop);

    for (int i = 0; i < lanes; ++i)
        th.emplace_back(lane_worker, &r, i, slice, align, r.file_size(), deadline, &res[(size_t) i]);
    for (auto & t : th) t.join();
    const double wall_s = std::chrono::duration<double>(clock_t_::now() - t0).count();
    stop.store(true); // lanes are done; do not let the load run past the measured window
    for (auto & t : loaders) t.join();

    long long bytes = 0, reads = 0, busy_ns = 0;
    for (const auto & x : res) {
        bytes += x.window_bytes;
        reads += x.reads;
        busy_ns += x.busy_ns;
    }
    const double mib = (double) bytes / (1024.0 * 1024.0);
    const double mibs = mib / wall_s;
    // Mean latency is per-read wall inside a lane; with N lanes busy, N*wall is the service budget,
    // so latency rising while throughput plateaus is the saturation signature.
    const double lat_ms = reads ? (double) busy_ns / (double) reads / 1e6 : 0.0;
    std::printf("%6d %12.1f %10.2f %9lld %11.2f %10s\n", lanes, mibs, lat_ms, reads, mib,
                r.direct() ? "direct" : "BUFFERED");
    std::fflush(stdout);
    r.close();
    if (mibs_out) *mibs_out = mibs;
    return true;
}

void usage(const char * a0) {
    std::fprintf(stderr,
                 "usage: %s --model PATH [--lanes 1,2,4,8,16] [--slice-kb N] [--seconds S] [--buffered]\n"
                 "  --slice-kb      bytes per read, default 4096 (a typical expert projection slice)\n"
                 "  --buffered      drop O_DIRECT, to see what the page cache contributes\n"
                 "  --compute-load  N CPU-burning threads alongside the lanes (default 0), to read\n"
                 "                  under the contention the streamer actually faces\n",
                 a0);
}

} // namespace

int main(int argc, char ** argv) {
    std::string model;
    std::string lanes_spec = "1,2,4,8,12,16,24,32";
    size_t slice_kb = 4096;
    double seconds = 5.0;
    bool direct = true;
    int load = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char * what) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--model" || a == "-m")
            model = next("--model");
        else if (a == "--lanes")
            lanes_spec = next("--lanes");
        else if (a == "--slice-kb")
            slice_kb = (size_t) std::atoll(next("--slice-kb"));
        else if (a == "--seconds")
            seconds = std::atof(next("--seconds"));
        else if (a == "--buffered")
            direct = false;
        else if (a == "--compute-load")
            load = std::atoi(next("--compute-load"));
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (model.empty()) {
        usage(argv[0]);
        return 2;
    }

    std::vector<int> lanes;
    for (size_t p = 0; p < lanes_spec.size();) {
        const size_t c = lanes_spec.find(',', p);
        const std::string tok = lanes_spec.substr(p, c == std::string::npos ? std::string::npos : c - p);
        if (!tok.empty()) lanes.push_back(std::atoi(tok.c_str()));
        if (c == std::string::npos) break;
        p = c + 1;
    }
    if (lanes.empty()) {
        std::fprintf(stderr, "no lane counts parsed\n");
        return 2;
    }

    const size_t slice = slice_kb * 1024;
    std::printf("model     %s\n", model.c_str());
    std::printf("read size %zu KiB, %.1f s per row, compute load %d threads\n\n", slice_kb, seconds, load);
    std::printf("%6s %12s %10s %9s %11s %10s\n", "lanes", "MiB/s", "lat_ms", "reads", "read_MiB", "mode");

    double best = 0.0;
    int best_lanes = 0;
    for (int L : lanes) {
        if (L < 1) continue;
        double mibs = 0.0;
        if (!run_row(model, L, slice, direct, seconds, load, &mibs)) return 1;
        if (mibs > best) {
            best = mibs;
            best_lanes = L;
        }
    }
    std::printf("\npeak %.1f MiB/s at %d lanes\n", best, best_lanes);
    return 0;
}
