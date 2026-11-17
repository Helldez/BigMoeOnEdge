#pragma once

// Pure half-open address-range coalescing used when a batch of cache evictions releases their
// pages. Keeping this free of the platform and the streamer makes the accounting honest: a merge
// must never turn two disjoint spans into one that covers memory between them. Header-only so the
// streamer and the standalone test share the exact code.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bmoe {

struct ByteRange {
    uintptr_t begin;
    uintptr_t end;
};

inline bool operator<(const ByteRange & a, const ByteRange & b) {
    if (a.begin != b.begin) return a.begin < b.begin;
    return a.end < b.end;
}

// Sort and fold touching/overlapping ranges into maximal disjoint ranges. Empty/inverted inputs
// are dropped; touching ranges (a.end == b.begin) are merged, which is what a page-granular
// vm_evict needs to issue one call instead of several.
inline std::vector<ByteRange> merge_ranges(std::vector<ByteRange> ranges) {
    std::sort(ranges.begin(), ranges.end());
    std::vector<ByteRange> out;
    out.reserve(ranges.size());
    for (const ByteRange r : ranges) {
        if (r.end <= r.begin) continue;
        if (out.empty() || r.begin > out.back().end)
            out.push_back(r);
        else if (r.end > out.back().end)
            out.back().end = r.end;
    }
    return out;
}

} // namespace bmoe
