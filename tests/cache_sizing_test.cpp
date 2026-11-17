// Pure sizing/range helpers (core/include/bmoe/cache_sizing.h, span_merge.h). No llama.cpp, no
// model: these are the arithmetic and page-accounting rules the cache budget rests on. Checks are
// explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.
#include "cache_sizing.h"
#include "span_merge.h"

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

int main() {
    using namespace bmoe;

    // One layer's full set is the largest across layers, not the sum: prefill stages one layer at
    // a time, so the budget only has to hold the widest layer's experts for its matmul.
    {
        const size_t per[] = {100, 500, 250};
        check(prefill_layer_floor_bytes(per, 3, 4, 100000) == 2000,
              "the floor is the widest layer's full set (500 * 4)");
        check(prefill_layer_floor_bytes(per, 3, 8, 100000) == 4000,
              "doubling the expert count doubles the floor");
        check(prefill_layer_floor_bytes(per, 3, 4, 1200) == 1200, "the floor is capped at the hard cap");
        check(prefill_layer_floor_bytes(per, 0, 4, 100000) == 0, "no layers means no floor");
        check(prefill_layer_floor_bytes(per, 3, 0, 100000) == 0, "no experts means no floor");

        const size_t huge[] = {std::numeric_limits<size_t>::max() / 2, 0, 0};
        check(prefill_layer_floor_bytes(huge, 3, 8, 4096) == 4096,
              "a saturating multiply cannot wrap the floor below the cap");
    }

    // Eviction coalescing must never widen a span into memory between two disjoint ranges, and must
    // fold touching page ranges into one vm_evict call.
    {
        std::vector<ByteRange> in = {{100, 200}, {300, 400}, {200, 300}, {600, 600}, {50, 90}};
        const auto out = merge_ranges(in);
        check(out.size() == 2, "three touching ranges merge into one; the isolated one stays");
        check(out.size() == 2 && out[0].begin == 50 && out[0].end == 90, "the isolated span is untouched");
        check(out.size() == 2 && out[1].begin == 100 && out[1].end == 400,
              "the merged span is exactly [100,400) - no widening into the gap");
    }
    {
        const auto empty = merge_ranges({});
        check(empty.empty(), "an empty batch yields no ranges");
        const auto one = merge_ranges({{7, 9}});
        check(one.size() == 1 && one[0].begin == 7 && one[0].end == 9, "a single range passes through");
        const auto overlap = merge_ranges({{0, 10}, {5, 20}, {19, 30}});
        check(overlap.size() == 1 && overlap[0].begin == 0 && overlap[0].end == 30,
              "overlapping ranges fold into their union");
    }

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall cache sizing checks passed\n");
    return 0;
}
