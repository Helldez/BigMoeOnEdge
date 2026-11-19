#include "context_fallback.h"

#include <algorithm>
#include <vector>

namespace bmoe::detail {

ContextOpened
open_with_fallbacks(const ContextAsk & first,
                    const ContextFloors & floors,
                    const std::function<bool(const ContextAsk &)> & open,
                    const std::function<void(const ContextAsk &, const ContextAsk &, ContextRetry)> & retry) {
    ContextOpened out;
    ContextAsk last = first;
    const auto ask = [&](const ContextAsk & a, ContextRetry why) {
        if (retry) retry(last, a, why);
        last = a;
        ++out.attempts;
        if (!open(a)) return false;
        out.ok = true;
        out.ask = a;
        return true;
    };
    ++out.attempts;
    if (open(first)) {
        out.ok = true;
        out.ask = first;
        return out;
    }

    // 1. The cache alone. Whole MiB, as the session records it.
    std::vector<size_t> ladder; // every size under the planned one, largest first
    if (first.cache_bytes > 0) {
        const size_t floor = std::max<size_t>(floors.cache_bytes, 1);
        for (size_t size = first.cache_bytes; size - size / 4 >= floor;) {
            const size_t smaller = (size - size / 4) & ~((size_t(1) << 20) - 1);
            if (smaller == 0 || smaller >= size) break;
            ladder.push_back(size = smaller);
        }
    }
    // The ladder beside whatever else `with` asks for: the largest cache that opens.
    const auto largest_cache = [&](ContextAsk with, ContextRetry why) {
        for (size_t size : ladder) {
            with.cache_bytes = size;
            if (ask(with, why)) return true;
        }
        return false;
    };
    if (largest_cache(first, ContextRetry::SmallerCache)) return out;

    // A later remedy is asked for with the cache as planned and, where that is refused, with the
    // smallest cache: the one ask that says whether the remedy helps at all. Where it does, the
    // cache did stand in the way too, and it climbs back to the largest size that opens beside the
    // remedy, which costs asks only on the way to a context that opens.
    const auto with_cache = [&](ContextAsk a, ContextRetry why) {
        a.cache_bytes = first.cache_bytes;
        if (ask(a, why)) return true;
        if (ladder.empty()) return false;
        a.cache_bytes = ladder.back();
        if (!ask(a, why)) return false;
        out.ok = false;
        return largest_cache(a, ContextRetry::SmallerCache);
    };

    // 2. The V cache alone.
    ContextAsk unquantized = first;
    unquantized.v_quantized = false;
    if (first.v_quantized && with_cache(unquantized, ContextRetry::UnquantizedV)) return out;

    // 3. The graph, narrower: each width with the V cache as planned, then unquantized.
    const uint32_t width_floor = std::max<uint32_t>(floors.width, 1);
    for (uint32_t width = first.width / 2; width >= width_floor; width /= 2) {
        ContextAsk a = first;
        a.width = width;
        if (with_cache(a, ContextRetry::NarrowerGraph)) return out;
        if (!first.v_quantized) continue;
        a.v_quantized = false;
        if (with_cache(a, ContextRetry::NarrowerGraph)) return out;
    }
    return out;
}

} // namespace bmoe::detail
