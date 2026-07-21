// bmoe-membench — can dense weights live in memory the kernel is not allowed to reclaim?
//
// The dense (non-expert) weights are the one part of the model that must stay resident: they are
// touched for every token, and when Android's kswapd evicts them the engine refaults them off flash
// on the next token. `mlock` cannot defend them — the vendor caps RLIMIT_MEMLOCK at 64 KiB, which is
// why `--lock-dense` was abandoned. The one allocation an unprivileged app CAN make that the kernel
// will not reclaim is a dma-buf: those pages stay pinned for the lifetime of the buffer because a
// device may DMA from them at any time. Userspace reaches dma-buf through AHardwareBuffer.
//
// That would be pointless if the pinned pages were slow to read, and they might be: gralloc decides
// per-allocation whether a buffer is CPU-cacheable, and uncached memory loses the cache line, the
// hardware prefetcher and most of the memory-level parallelism. A dense matmul streams weights, so
// it is fully exposed to that. This tool answers the only question that gates the whole idea:
//
//     is a locked AHardwareBuffer BLOB readable at cached-DRAM speed, or at uncached speed?
//
// The two outcomes are an order of magnitude apart, so the measurement does not need to be careful —
// it needs to be unambiguous. Interpretation, on a device whose flash serves 1.3-2.5 GB/s:
//
//   ~10+ GB/s   cacheable. Indistinguishable from ordinary anonymous memory; pinning is viable and
//               the next step is the in-app A/B against `--dense-weights anon`.
//   ~1 GB/s or  uncached. At or below the flash bandwidth this is meant to save, so pinned dense
//   below      weights would be SLOWER than refaulting them — the idea is dead, not merely weaker.
//
// `anon` is the baseline on purpose: it is the allocation `--dense-weights anon` already makes, so
// the ratio between the two rows is the whole result. An absolute number alone would be worthless,
// since it would fold in the device's clocks and thermal state.
//
// Secondary output: `--probe-max` reports the largest BLOB the driver will actually hand over. No
// limit is documented — the NDK only says allocation may fail on driver constraints — and the dense
// working set is multiple GiB, so "does it even fit" has to be measured too. Note the format's own
// ceiling: AHardwareBuffer_Desc::width is 32-bit and for BLOB it IS the byte count, so 4 GiB - 1 is
// the hard cap regardless of what the driver would allow.
//
// Android-only for the `ahwb` mode; the `anon` mode runs anywhere, which keeps the tool buildable
// and testable on the host. Nothing here links the engine or llama.cpp.
//
//   bmoe-membench [--mib N] [--modes anon,ahwb] [--seconds S] [--threads N]
//                 [--usage often|rarely] [--probe-max]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#define BMOE_HAVE_AHWB 1
#else
#define BMOE_HAVE_AHWB 0
#endif

#if defined(_WIN32)
#include <malloc.h>
#else
#include <sys/mman.h>
#endif

namespace {

using clock_t_ = std::chrono::steady_clock;

// Sequential read of an 8-byte-aligned range, accumulating so the loads cannot be dead-coded. Four
// independent accumulators keep several loads in flight per iteration: on cacheable memory that is
// what lets the core reach DRAM bandwidth, and on uncached memory it is exactly what stops working —
// which is the difference this tool is built to expose, so the kernel must be written to benefit
// from it rather than serialised by hand.
uint64_t read_range(const uint8_t * base, size_t bytes) {
    const uint64_t * q = reinterpret_cast<const uint64_t *>(base);
    const size_t n = bytes / sizeof(uint64_t);
    uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        a0 += q[i];
        a1 += q[i + 1];
        a2 += q[i + 2];
        a3 += q[i + 3];
    }
    for (; i < n; ++i)
        a0 += q[i];
    return a0 + a1 + a2 + a3;
}

// Writes a non-zero pattern over the whole buffer. Not optional: a fresh anonymous mapping is all
// copy-on-write references to the shared zero page, so reading it without faulting pages in first
// would measure one cached page instead of DRAM.
void fill_range(uint8_t * base, size_t bytes, uint64_t seed) {
    uint64_t * q = reinterpret_cast<uint64_t *>(base);
    const size_t n = bytes / sizeof(uint64_t);
    for (size_t i = 0; i < n; ++i)
        q[i] = seed + i * 1099511628211ULL;
}

void parallel_over(uint8_t * base, size_t bytes, int threads, bool write, std::atomic<uint64_t> * sink) {
    if (threads <= 1) {
        if (write)
            fill_range(base, bytes, 0x9E3779B97F4A7C15ULL);
        else
            sink->fetch_add(read_range(base, bytes), std::memory_order_relaxed);
        return;
    }
    // Split on a 64-byte boundary so no two threads share a cache line at the seam.
    const size_t chunk = ((bytes / (size_t) threads) + 63) & ~(size_t) 63;
    std::vector<std::thread> th;
    th.reserve((size_t) threads);
    for (int t = 0; t < threads; ++t) {
        const size_t off = chunk * (size_t) t;
        if (off >= bytes) break;
        const size_t len = std::min(chunk, bytes - off);
        th.emplace_back([=]() {
            if (write)
                fill_range(base + off, len, 0x9E3779B97F4A7C15ULL + off);
            else
                sink->fetch_add(read_range(base + off, len), std::memory_order_relaxed);
        });
    }
    for (auto & t : th)
        t.join();
}

// One measured row: fill once, then read the buffer end to end repeatedly until the time budget is
// spent. Best-pass is the headline because it is the least contaminated by scheduler noise and by
// whatever else the device decided to run; the mean is printed alongside so a large gap between the
// two is visible rather than hidden.
struct BwResult {
    double best_mibs = 0.0;
    double mean_mibs = 0.0;
    int passes = 0;
};

BwResult measure(uint8_t * base, size_t bytes, double seconds, int threads) {
    std::atomic<uint64_t> sink{0};
    parallel_over(base, bytes, threads, true, &sink);

    BwResult r;
    const double mib = (double) bytes / (1024.0 * 1024.0);
    const auto t_end = clock_t_::now() + std::chrono::milliseconds((long long) (seconds * 1000.0));
    double total_s = 0.0;
    while (clock_t_::now() < t_end) {
        const auto t0 = clock_t_::now();
        parallel_over(base, bytes, threads, false, &sink);
        const double s = std::chrono::duration<double>(clock_t_::now() - t0).count();
        if (s <= 0.0) continue;
        total_s += s;
        r.passes++;
        r.best_mibs = std::max(r.best_mibs, mib / s);
    }
    if (total_s > 0.0) r.mean_mibs = mib * (double) r.passes / total_s;
    // Keep the accumulated sum observable so the read loop cannot be optimised away entirely.
    if (sink.load() == 0x1234567890ABCDEFULL) std::fprintf(stderr, "(checksum coincidence)\n");
    return r;
}

// --- allocators -------------------------------------------------------------------
// Each returns a base pointer or nullptr, and records how long the allocation itself took: a pinned
// multi-GiB allocation may be slow to service even when it succeeds, and that cost would be paid at
// model load, so it belongs in the report.

struct Alloc {
    uint8_t * base = nullptr;
    double alloc_ms = 0.0;
    std::string note;
};

Alloc alloc_anon(size_t bytes) {
    Alloc a;
    const auto t0 = clock_t_::now();
#if defined(_WIN32)
    a.base = (uint8_t *) _aligned_malloc(bytes, 4096);
#else
    void * p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    a.base = (p == MAP_FAILED) ? nullptr : (uint8_t *) p;
#endif
    a.alloc_ms = std::chrono::duration<double, std::milli>(clock_t_::now() - t0).count();
    return a;
}

void free_anon(uint8_t * p, size_t bytes) {
    if (!p) return;
#if defined(_WIN32)
    (void) bytes;
    _aligned_free(p);
#else
    munmap(p, bytes);
#endif
}

#if BMOE_HAVE_AHWB
AHardwareBuffer * g_ahwb = nullptr;

// BLOB is the "just N bytes" format: width carries the byte count and height must be 1. The usage
// flags are the whole experiment — CPU_READ_OFTEN is the request for a cacheable mapping, and
// whether gralloc honours it is exactly what is unknown.
Alloc alloc_ahwb(size_t bytes, bool read_often) {
    Alloc a;
    if (bytes > 0xFFFFFFFFULL) {
        a.note = "exceeds the 32-bit BLOB width cap (4 GiB)";
        return a;
    }
    AHardwareBuffer_Desc d{};
    d.width = (uint32_t) bytes;
    d.height = 1;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_BLOB;
    d.usage = (read_often ? AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN : AHARDWAREBUFFER_USAGE_CPU_READ_RARELY) |
              AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

    const auto t0 = clock_t_::now();
    AHardwareBuffer * buf = nullptr;
    const int rc = AHardwareBuffer_allocate(&d, &buf);
    if (rc != 0 || !buf) {
        a.alloc_ms = std::chrono::duration<double, std::milli>(clock_t_::now() - t0).count();
        a.note = "AHardwareBuffer_allocate failed (rc=" + std::to_string(rc) + ")";
        return a;
    }
    void * p = nullptr;
    const int lrc = AHardwareBuffer_lock(buf, d.usage, -1, nullptr, &p);
    a.alloc_ms = std::chrono::duration<double, std::milli>(clock_t_::now() - t0).count();
    if (lrc != 0 || !p) {
        AHardwareBuffer_release(buf);
        a.note = "AHardwareBuffer_lock failed (rc=" + std::to_string(lrc) + ")";
        return a;
    }
    g_ahwb = buf;
    a.base = (uint8_t *) p;
    return a;
}

void free_ahwb() {
    if (!g_ahwb) return;
    AHardwareBuffer_unlock(g_ahwb, nullptr);
    AHardwareBuffer_release(g_ahwb);
    g_ahwb = nullptr;
}

// Largest BLOB the driver will actually allocate: double until it refuses, then binary-search the
// gap. Allocation only — no lock, no touch — so this reports what the allocator promises, which is
// an upper bound on what is usable. It is deliberately separate from the bandwidth rows: a size that
// allocates but is unusably slow is not a size that helps.
void probe_max() {
    const uint64_t cap = 0xFFFFFFFFULL; // 32-bit BLOB width
    auto can_alloc = [](uint64_t bytes) -> bool {
        if (bytes > 0xFFFFFFFFULL) return false;
        AHardwareBuffer_Desc d{};
        d.width = (uint32_t) bytes;
        d.height = 1;
        d.layers = 1;
        d.format = AHARDWAREBUFFER_FORMAT_BLOB;
        d.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
        AHardwareBuffer * b = nullptr;
        if (AHardwareBuffer_allocate(&d, &b) != 0 || !b) return false;
        AHardwareBuffer_release(b);
        return true;
    };

    std::printf("probing the largest allocatable BLOB (32-bit width caps this at 4096 MiB)\n");
    uint64_t good = 0, bad = 0;
    for (uint64_t mib = 64; mib * 1024 * 1024 <= cap; mib *= 2) {
        const uint64_t bytes = mib * 1024 * 1024;
        const bool ok = can_alloc(bytes);
        std::printf("  %6llu MiB  %s\n", (unsigned long long) mib, ok ? "ok" : "FAILED");
        std::fflush(stdout);
        if (ok) {
            good = bytes;
        } else {
            bad = bytes;
            break;
        }
    }
    if (bad == 0) {
        std::printf("  every probed size allocated; ceiling is the 4 GiB format cap, not the driver\n\n");
        return;
    }
    // 16 MiB is fine enough: the decision this informs is "does several GiB fit", not the exact byte.
    while (bad - good > 16ULL * 1024 * 1024) {
        const uint64_t mid = good + (bad - good) / 2;
        if (can_alloc(mid))
            good = mid;
        else
            bad = mid;
    }
    std::printf("  max allocatable ~%llu MiB\n\n", (unsigned long long) (good / (1024 * 1024)));
}
#endif // BMOE_HAVE_AHWB

void usage(const char * a0) {
    std::fprintf(stderr,
                 "usage: %s [--mib N] [--modes anon,ahwb] [--seconds S] [--threads N]\n"
                 "  --mib       buffer size, default 512 (must exceed the last-level cache, or the\n"
                 "              read loop measures cache instead of DRAM)\n"
                 "  --modes     comma list of anon,ahwb (default both; ahwb is Android-only)\n"
                 "  --threads   readers running concurrently, default 1. Single-thread separates\n"
                 "              cached from uncached most clearly; the multi-thread row is the one\n"
                 "              comparable to a matmul, which reads from every compute thread.\n"
                 "  --usage     often|rarely (default often) — the CPU-read hint given to gralloc.\n"
                 "              If both give the same bandwidth, the hint is being ignored.\n"
                 "  --probe-max report the largest allocatable BLOB, then exit\n",
                 a0);
}

} // namespace

int main(int argc, char ** argv) {
    size_t mib = 512;
    std::string modes = "anon,ahwb";
    double seconds = 3.0;
    int threads = 1;
    bool read_often = true;
    bool probe = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char * what) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--mib")
            mib = (size_t) std::atoll(next("--mib"));
        else if (a == "--modes")
            modes = next("--modes");
        else if (a == "--seconds")
            seconds = std::atof(next("--seconds"));
        else if (a == "--threads")
            threads = std::atoi(next("--threads"));
        else if (a == "--usage")
            read_often = std::string(next("--usage")) != "rarely";
        else if (a == "--probe-max")
            probe = true;
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (mib == 0 || threads < 1) {
        usage(argv[0]);
        return 2;
    }

    if (probe) {
#if BMOE_HAVE_AHWB
        probe_max();
        return 0;
#else
        std::fprintf(stderr, "--probe-max needs AHardwareBuffer (Android build)\n");
        return 2;
#endif
    }

    const size_t bytes = mib * 1024 * 1024;
    std::printf("buffer %zu MiB, %.1f s per mode, %d reader thread(s), cpu-read hint '%s'\n\n", mib, seconds, threads,
                read_often ? "often" : "rarely");
    std::printf("%-8s %12s %12s %10s %8s  %s\n", "mode", "best_MiB/s", "mean_MiB/s", "alloc_ms", "passes", "note");

    double anon_best = 0.0, ahwb_best = 0.0;
    const bool want_anon = modes.find("anon") != std::string::npos;
    const bool want_ahwb = modes.find("ahwb") != std::string::npos;

    if (want_anon) {
        Alloc a = alloc_anon(bytes);
        if (!a.base) {
            std::printf("%-8s %12s %12s %10.1f %8s  %s\n", "anon", "-", "-", a.alloc_ms, "-", "allocation failed");
        } else {
            const BwResult r = measure(a.base, bytes, seconds, threads);
            anon_best = r.best_mibs;
            std::printf("%-8s %12.1f %12.1f %10.1f %8d  %s\n", "anon", r.best_mibs, r.mean_mibs, a.alloc_ms, r.passes,
                        "reclaimable baseline");
            free_anon(a.base, bytes);
        }
    }

    if (want_ahwb) {
#if BMOE_HAVE_AHWB
        Alloc a = alloc_ahwb(bytes, read_often);
        if (!a.base) {
            std::printf("%-8s %12s %12s %10.1f %8s  %s\n", "ahwb", "-", "-", a.alloc_ms, "-", a.note.c_str());
        } else {
            const BwResult r = measure(a.base, bytes, seconds, threads);
            ahwb_best = r.best_mibs;
            std::printf("%-8s %12.1f %12.1f %10.1f %8d  %s\n", "ahwb", r.best_mibs, r.mean_mibs, a.alloc_ms, r.passes,
                        "pinned dma-buf");
            free_ahwb();
        }
#else
        std::printf("%-8s %12s %12s %10s %8s  %s\n", "ahwb", "-", "-", "-", "-", "not an Android build");
#endif
    }

    if (anon_best > 0.0 && ahwb_best > 0.0) {
        const double ratio = ahwb_best / anon_best;
        std::printf("\nahwb / anon = %.2fx — ", ratio);
        // The verdict thresholds are wide because the two outcomes are an order of magnitude apart;
        // anything in between is a real result that needs a human, not a rounding decision.
        if (ratio >= 0.7)
            std::printf("cacheable. Pinning is viable; next step is the in-app A/B.\n");
        else if (ratio >= 0.2)
            std::printf("degraded but not uncached. Weigh against the refault cost before building.\n");
        else
            std::printf("uncached. Pinned dense weights would read slower than the flash they replace.\n");
    }
    return 0;
}
