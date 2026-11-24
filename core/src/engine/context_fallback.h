#pragma once

// What a context is asked for again when llama.cpp refuses to make it.
//
// Three things can stop a context from opening, and none can be told from the others before
// asking: a device expert cache that leaves the context's own buffers no room, a quantized V cache
// on a backend without flash attention, and a graph too wide for the memory left. Each has its own
// remedy, and a remedy is paid for: a smaller cache holds fewer experts for the whole session.
//
// So each remedy is first tried ALONE, with the cache as planned. They used to be tried in order
// with everything kept from the one before: where the V cache was the reason, the cache had already
// been cut to one token cycle by the time the V cache was asked about, the context opened like
// that, and the session went on reporting the size it had planned. A planned 15 GB cache could end
// at a few hundred MiB for a reason that had nothing to do with memory.
//
// Pure policy: the session supplies the attempt (a context creation) and reads back what opened.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace bmoe::detail {

struct ContextAsk {
    size_t cache_bytes = 0;   // the device expert cache, 0 where none is armed
    bool v_quantized = false; // the V cache's type is a quantized one
    uint32_t width = 0;       // tokens per pass (n_ubatch)

    bool operator==(const ContextAsk & o) const {
        return cache_bytes == o.cache_bytes && v_quantized == o.v_quantized && width == o.width;
    }
};

struct ContextFloors {
    size_t cache_bytes = 0; // one token cycle: under it a cache returns no hits (0: no floor known)
    uint32_t width = 0;     // llama.cpp's own physical batch, or the most tokens one image takes
};

// Why an ask differs from the one before it, for the session's log.
enum class ContextRetry { SmallerCache, UnquantizedV, NarrowerGraph };

struct ContextOpened {
    bool ok = false;
    ContextAsk ask; // what the context opened with, where ok
    int attempts = 0;
};

// Asks `open` for a context with `first`, then with each remedy until one opens. `retry` is told
// of every ask after the first, before it is made. `open` may be asked again after it has
// succeeded (the climb below), so it lets go of the context it made before making another; the ask
// returned is always the last one it opened.
//   1. the cache alone, a quarter less at a time down to its floor;
//   2. the V cache alone, unquantized;
//   3. the graph, half as wide at a time down to its floor, each width with the V cache as planned
//      and then unquantized.
// Steps 2 and 3 ask with the cache as planned first. Where that is refused they ask once with the
// smallest cache of step 1, and where that opens the cache was in the way as well: it climbs back
// to the largest size that opens beside the remedy.
ContextOpened open_with_fallbacks(
    const ContextAsk & first,
    const ContextFloors & floors,
    const std::function<bool(const ContextAsk &)> & open,
    const std::function<void(const ContextAsk & from, const ContextAsk & to, ContextRetry why)> & retry = nullptr);

} // namespace bmoe::detail
