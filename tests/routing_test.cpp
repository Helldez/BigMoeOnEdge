// Unit tests for the routing trace the planner reads (core/src/plan/routing.cpp).
//
// The replay is the planner's hit rate, so it has to be the engine's: it is checked to the third
// decimal against scripts/route-replay.py's LRU row on the three phone traces archived in
// docs/bench-data/2026-07-15-route-trace, whose cumulative figures at the run's own budget are the
// hit rates those runs recorded (74.9, 76.8 and 32.4%). The expected values below are the script's
// output (`--policies lru --budgets 500,1000,2000,3000,4000 --csv`), not this file's.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG.

#include "bmoe/routing.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace bmoe;

static int failures = 0;

static void check(bool cond, const std::string & name, const std::string & detail = "") {
    if (cond) {
        std::printf("[PASS] %s\n", name.c_str());
    } else {
        std::printf("[FAIL] %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str());
        ++failures;
    }
}

static std::string slurp(const std::string & path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

static const double MiB = 1024.0 * 1024.0;

struct Expected {
    double budget_mib, hit_pct_cum, hit_pct_decode, read_mib;
};

static void replay_matches_script(const char * tag,
                                  const std::vector<Expected> & rows,
                                  double run_budget_mib,
                                  double recorded_pct) {
    const std::string path = std::string(BMOE_ROUTE_TRACE_DIR) + "/" + tag + ".route.csv";
    RoutingTrace t;
    std::string err;
    const bool ok = parse_routing_trace(slurp(path), t, err);
    check(ok, std::string(tag) + ": the archived --route-trace parses", err);
    if (!ok) return;
    for (const Expected & e : rows) {
        const ReplayCounts r = replay_lru(t, (uint64_t) (e.budget_mib * MiB));
        char got[160];
        std::snprintf(got, sizeof(got), "%.3f / %.3f / %.1f against %.3f / %.3f / %.1f", 100.0 * r.hit(),
                      100.0 * r.decode_hit(), r.read_bytes / MiB, e.hit_pct_cum, e.hit_pct_decode, e.read_mib);
        check(std::fabs(100.0 * r.hit() - e.hit_pct_cum) < 0.0005 &&
                  std::fabs(100.0 * r.decode_hit() - e.hit_pct_decode) < 0.0005 &&
                  std::fabs(r.read_bytes / MiB - e.read_mib) < 0.05,
              std::string(tag) + ": LRU replay at " + std::to_string((int) e.budget_mib) +
                  " MiB matches route-replay.py",
              got);
    }
    const ReplayCounts run = replay_lru(t, (uint64_t) (run_budget_mib * MiB));
    char got[64];
    std::snprintf(got, sizeof(got), "%.1f%%", 100.0 * run.hit());
    check(std::fabs(std::round(1000.0 * run.hit()) / 10.0 - recorded_pct) < 1e-9,
          std::string(tag) + ": the replay reproduces the hit rate the run recorded", got);

    // The curve: from nothing to the whole set, non-decreasing, and at the run's budget where the
    // replay put it (interpolation between two points never leaves the two points' range).
    const HitCurve c = hit_curve(t, k_hit_curve_points);
    bool mono = c.measured && c.points.size() == k_hit_curve_points + 1 && c.points.front().cache_bytes == 0;
    for (size_t i = 1; i < c.points.size(); ++i)
        mono = mono && c.points[i].hit >= c.points[i - 1].hit && c.points[i].cache_bytes > c.points[i - 1].cache_bytes;
    check(mono, std::string(tag) + ": the curve is non-decreasing from 0 to the whole set");
    check(c.points.back().cache_bytes == t.expert_set_bytes(), std::string(tag) + ": the curve ends at the expert set");
    bool share_mono = true;
    double last = -1.0;
    for (uint64_t b = 0; b <= t.expert_set_bytes() + (64ull << 20); b += 37ull << 20) {
        const double h = c.hit_share(b);
        share_mono = share_mono && h >= last && h >= 0.0 && h <= 1.0;
        last = h;
    }
    check(share_mono, std::string(tag) + ": hit_share is monotone in [0, 1] at any size");
}

int main() {
    replay_matches_script("qwen",
                          {{500, 0.000, 0.000, 107715.0},
                           {1000, 35.141, 37.679, 70208.5},
                           {2000, 52.740, 56.548, 51252.3},
                           {3000, 65.858, 70.614, 37007.0},
                           {4000, 74.880, 80.287, 27213.7}},
                          4000, 74.9);
    replay_matches_script("gemma",
                          {{500, 0.000, 0.000, 92362.0},
                           {1000, 33.819, 36.076, 61308.0},
                           {2000, 58.268, 62.157, 38651.3},
                           {3000, 68.525, 73.099, 29175.9},
                           {4000, 76.784, 81.910, 21539.7}},
                          4000, 76.8);
    replay_matches_script("gptoss",
                          {{500, 0.000, 0.000, 80368.4},
                           {1000, 12.769, 17.665, 70106.5},
                           {2000, 22.322, 30.881, 62428.9},
                           {3000, 32.408, 44.835, 54322.8},
                           {4000, 37.380, 51.714, 50326.4}},
                          3000, 32.4);

    // ── a small trace, by hand ──────────────────────────────────────────────────────────────
    // Two blocks of four experts, 10 bytes each. A prompt of two tokens, then three decoded ones.
    RoutingTrace t;
    t.n_layer = 2;
    t.n_expert = 4;
    t.n_expert_used = 2;
    t.expert_bytes = {10, 10};
    t.prefill_tokens = 2;
    t.decode_tokens = 3;
    t.batches = {
        {0, false, {0, 1, 1, 2}}, {1, false, {3, 0, 3, 1}}, // the prompt: one batch per block
        {0, true, {0, 1}},        {1, true, {3, 0}},        // token 1
        {0, true, {1, 2}},        {1, true, {3, 2}},        // token 2
        {0, true, {0, 1}},        {1, true, {3, 1}},        // token 3
    };

    // A repeat inside a batch is not a lookup: 3 + 3 prompt lookups, 12 decode ones.
    {
        const ReplayCounts all = replay_lru(t, 80);
        check(all.lookups == 18 && all.decode_lookups == 12, "a repeat in a batch promotes and is not looked up",
              std::to_string(all.lookups) + " / " + std::to_string(all.decode_lookups));
        // Everything fits: the 6 prompt experts and the one new expert (1:2) are the only misses.
        check(all.hits == 11 && all.decode_hits == 11 && all.read_bytes == 70,
              "a cache that holds the set misses each expert once",
              std::to_string(all.hits) + " hits, " + std::to_string(all.read_bytes) + " bytes");
        // A budget of nothing keeps only the batch's own entries until the next batch.
        const ReplayCounts none = replay_lru(t, 0);
        check(none.hits == 0, "a budget of nothing serves nothing", std::to_string(none.hits));
    }

    // The text form reads back as the same trace, and so replays the same.
    {
        RoutingTrace back;
        std::string err;
        const bool ok = parse_routing_trace(format_routing_trace(t), back, err);
        check(ok && back.batches.size() == t.batches.size() && back.decode_tokens == 3 && back.prefill_tokens == 2 &&
                  back.n_expert_used == 2 && back.expert_bytes == t.expert_bytes,
              "format -> parse round-trips", err);
        bool same = ok;
        for (size_t i = 0; same && i < t.batches.size(); ++i)
            same = back.batches[i].layer == t.batches[i].layer && back.batches[i].decode == t.batches[i].decode &&
                   back.batches[i].experts == t.batches[i].experts;
        check(same, "format -> parse keeps every batch as it was");
        for (uint64_t b : {0ull, 20ull, 30ull, 50ull, 80ull})
            check(replay_lru(back, b).hits == replay_lru(t, b).hits,
                  "a read-back trace replays the same at " + std::to_string((unsigned long long) b));
        RoutingTrace bad;
        check(!parse_routing_trace("hello\n", bad, err), "text that is not a trace is refused", err);
        check(!parse_routing_trace("# bmoe routing v1\n# n_layer=1 n_expert=4 n_expert_used=1\np 0 1\n", bad, err),
              "a layer without expert bytes is refused, never priced as free", err);
    }

    // The hottest experts: every routing is a use, ranked by uses, ties by block then id.
    {
        const std::vector<HotExpert> hot = hottest_experts(t);
        // block 0: e0 3, e1 5, e2 2; block 1: e3 5, e0 2, e1 2, e2 1.
        check(hot.size() == 7, "every expert used is listed", std::to_string(hot.size()));
        check(hot.size() == 7 && hot[0].layer == 0 && hot[0].expert == 1 && hot[0].uses == 5 && hot[1].layer == 1 &&
                  hot[1].expert == 3 && hot[1].uses == 5 && hot[2].layer == 0 && hot[2].expert == 0 && hot[2].uses == 3,
              "ranked by uses, a tie by block then expert id");
        const std::vector<HotExpert> top2 = hottest_experts(t, 2);
        check(top2.size() == 2 && top2[0].expert == 1 && top2[1].expert == 3, "a limit keeps the hottest");
        uint64_t total = 0;
        for (const HotExpert & h : hot)
            total += h.uses;
        check(total == 20, "the uses add up to every routing in the trace", std::to_string(total));
    }

    // attach_routing: a trace is this model's only if it matches the file and the run.
    {
        ModelProfile m;
        m.ok = true;
        m.is_moe = true;
        m.n_expert = 4;
        m.n_expert_used = 2;
        m.moe_layers = {0, 1};
        m.expert_bytes = 80;
        RoutingTrace longer = t;
        for (int i = 0; i < 40; ++i) {
            longer.batches.push_back({0, true, {0, 1}});
            longer.batches.push_back({1, true, {3, 0}});
        }
        longer.decode_tokens = 43;
        attach_routing(m, longer, 2);
        check(m.hit_curve.measured && m.hit_curve.rejected.empty(), "a matching trace gives the model its curve",
              m.hit_curve.rejected);
        check(m.hit_curve.hit_share(80) > 0.9, "the whole set serves nearly every decode lookup");
        attach_routing(m, longer, 1);
        check(!m.hit_curve.measured && !m.hit_curve.rejected.empty(), "another top-k is another routing: refused",
              m.hit_curve.rejected);
        attach_routing(m, t, 2);
        check(!m.hit_curve.measured && !m.hit_curve.rejected.empty(), "a trace too short to be a curve is refused",
              m.hit_curve.rejected);
        ModelProfile other = m;
        other.expert_bytes = 160;
        attach_routing(other, longer, 2);
        check(!other.hit_curve.measured, "another model's expert set is refused", other.hit_curve.rejected);
        other = m;
        other.moe_layers = {0, 1, 2};
        attach_routing(other, longer, 2);
        check(!other.hit_curve.measured, "another model's blocks are refused", other.hit_curve.rejected);
        check(HitCurve{}.hit_share(1ull << 40) == 0.0, "an absent curve credits nothing by itself");
    }

    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
