// The order a refused context is asked for again (context_fallback.h), against machines that refuse
// for one reason or for two. What is checked is what the context opens WITH: a remedy for one
// reason must not cost the session the device cache that another reason never touched.

#include "../core/src/engine/context_fallback.h"

#include <cstdio>
#include <string>

using namespace bmoe::detail;

namespace {

int failures = 0;

void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

constexpr size_t MiB = size_t(1) << 20;

// A machine: the most device memory a cache and a graph may take together, and whether its backend
// opens a quantized V cache at all.
struct Machine {
    size_t device_bytes = 0;
    size_t bytes_per_width = 0; // the graph's buffers, per token of width
    bool quantized_v = true;
    bool opens(const ContextAsk & a) const {
        if (a.v_quantized && !quantized_v) return false;
        return a.cache_bytes + (size_t) a.width * bytes_per_width <= device_bytes;
    }
};

ContextOpened run(const Machine & m, const ContextAsk & first, const ContextFloors & floors) {
    return open_with_fallbacks(first, floors, [&](const ContextAsk & a) { return m.opens(a); });
}

} // namespace

int main() {
    const ContextAsk planned{15000 * MiB, true, 2048};
    const ContextFloors floors{600 * MiB, 512};
    const size_t per_width = MiB; // 2048 wide: 2 GiB of buffers

    // Nothing in the way: one ask, nothing changed.
    {
        const ContextOpened o = run({20000 * MiB, per_width, true}, planned, floors);
        check(o.ok && o.attempts == 1 && o.ask == planned, "a context that opens is asked for once, as planned");
    }
    // The cache alone: it shrinks until it fits, and nothing else moves.
    {
        const ContextOpened o = run({12000 * MiB, per_width, true}, planned, floors);
        check(o.ok && o.ask.v_quantized && o.ask.width == 2048 && o.ask.cache_bytes < planned.cache_bytes &&
                  o.ask.cache_bytes + 2048 * per_width <= 12000 * MiB && o.ask.cache_bytes >= 7000 * MiB,
              "cache too large: it gives up quarters until it fits, and V and the width stand (" +
                  std::to_string(o.ask.cache_bytes / MiB) + " MiB)");
        check(o.ask.cache_bytes % MiB == 0, "cache too large: what opens is whole MiB, as the session records it");
    }
    // The V cache alone, on a backend without flash attention. This is the case that used to end
    // with the cache at its floor: the cache was shrunk first and never given back.
    {
        const ContextOpened o = run({20000 * MiB, per_width, false}, planned, floors);
        check(o.ok && !o.ask.v_quantized && o.ask.cache_bytes == planned.cache_bytes && o.ask.width == 2048,
              "V refused: V goes to f16 and the device cache stays as planned (" +
                  std::to_string(o.ask.cache_bytes / MiB) + " MiB)");
    }
    // The graph alone, with no cache armed: it halves to the first width that fits, V as planned.
    {
        const ContextAsk no_cache{0, true, 4096};
        const ContextOpened o = run({1500 * MiB, per_width, true}, no_cache, floors);
        check(o.ok && o.ask.width == 1024 && o.ask.v_quantized && o.ask.cache_bytes == 0,
              "graph too wide, no cache: the first width that fits, and V stays as planned (" +
                  std::to_string(o.ask.width) + ")");
    }
    // Two reasons at once: V refused and the cache too large. The cache ends where it would have
    // with V never in the way, not at its floor, and the context that is open is the last one asked.
    {
        const Machine both{12000 * MiB, per_width, false};
        const ContextOpened alone = run({12000 * MiB, per_width, true}, planned, floors);
        ContextAsk last_opened;
        int open_now = 0;
        const ContextOpened o = open_with_fallbacks(planned, floors, [&](const ContextAsk & a) {
            open_now = 0; // the session frees the one before
            if (!both.opens(a)) return false;
            last_opened = a;
            open_now = 1;
            return true;
        });
        check(o.ok && !o.ask.v_quantized && o.ask.width == 2048 && o.ask.cache_bytes == alone.ask.cache_bytes,
              "V refused and cache too large: V at f16 and the largest cache that fits (" +
                  std::to_string(o.ask.cache_bytes / MiB) + " MiB)");
        check(open_now == 1 && last_opened == o.ask, "V refused and cache too large: what is returned is what is open");
    }
    // The graph and the cache: the first width a cache fits beside, and the largest cache there.
    {
        const ContextOpened o = run({1200 * MiB, per_width, true}, planned, floors);
        check(o.ok && o.ask.v_quantized && o.ask.width == 512 && o.ask.cache_bytes >= floors.cache_bytes &&
                  o.ask.cache_bytes + 512 * per_width <= 1200 * MiB,
              "graph too wide and cache too large: the first width a cache fits beside (" +
                  std::to_string(o.ask.width) + " wide, " + std::to_string(o.ask.cache_bytes / MiB) + " MiB)");
    }
    // The cache is never asked for under its floor, and a machine nothing fits on is a failure.
    {
        size_t least = planned.cache_bytes;
        uint32_t narrowest = planned.width;
        const ContextOpened o = open_with_fallbacks(planned, floors, [&](const ContextAsk & a) {
            least = a.cache_bytes < least ? a.cache_bytes : least;
            narrowest = a.width < narrowest ? a.width : narrowest;
            return false;
        });
        check(!o.ok, "nothing opens: reported as a failure");
        check(least >= floors.cache_bytes && narrowest >= floors.width,
              "nothing opens: never asked under one token cycle (" + std::to_string(least / MiB) +
                  " MiB) or under the narrowest graph (" + std::to_string(narrowest) + ")");
        check(o.attempts < 40, "nothing opens: a bounded number of asks (" + std::to_string(o.attempts) + ")");
    }
    // Every ask after the first is announced, with what it changes.
    {
        int said = 0, cache = 0, v = 0, graph = 0;
        const ContextOpened o = open_with_fallbacks(
            planned, floors, [](const ContextAsk &) { return false; },
            [&](const ContextAsk &, const ContextAsk &, ContextRetry why) {
                ++said;
                cache += why == ContextRetry::SmallerCache;
                v += why == ContextRetry::UnquantizedV;
                graph += why == ContextRetry::NarrowerGraph;
            });
        check(said == o.attempts - 1 && cache > 0 && v == 2 && graph == 8,
              "every retry is announced: " + std::to_string(cache) + " cache, " + std::to_string(v) + " V, " +
                  std::to_string(graph) + " graph");
    }
    return failures ? 1 : 0;
}
