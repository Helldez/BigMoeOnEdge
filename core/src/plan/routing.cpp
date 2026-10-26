// The routing trace, its text form, the LRU replay and what is read from it. Read bmoe/routing.h
// first: it says why the planner reads a model's routing at all.

#include "bmoe/routing.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace bmoe {

uint64_t RoutingTrace::expert_set_bytes() const {
    std::vector<bool> seen(expert_bytes.size(), false);
    uint64_t total = 0;
    for (const RoutingBatch & b : batches) {
        if (b.layer >= expert_bytes.size() || seen[b.layer]) continue;
        seen[b.layer] = true;
        total += expert_bytes[b.layer] * n_expert;
    }
    return total;
}

double HitCurve::hit_share(uint64_t cache_bytes) const {
    if (!measured || points.empty()) return 0.0;
    if (cache_bytes <= points.front().cache_bytes) return points.front().hit;
    for (size_t i = 1; i < points.size(); ++i) {
        const HitPoint & a = points[i - 1];
        const HitPoint & b = points[i];
        if (cache_bytes > b.cache_bytes) continue;
        const double span = (double) (b.cache_bytes - a.cache_bytes);
        const double t = span > 0.0 ? (double) (cache_bytes - a.cache_bytes) / span : 1.0;
        return a.hit + t * (b.hit - a.hit);
    }
    return points.back().hit;
}

// ── text ─────────────────────────────────────────────────────────────────────────────────────
namespace {

constexpr const char * k_native_magic = "# bmoe routing v1";
constexpr const char * k_route_trace_magic = "# route_trace v1";

// `key=value` tokens of a comment line, as scripts/trace_io.py reads them.
std::map<std::string, std::string> kv_tokens(const std::string & line) {
    std::map<std::string, std::string> kv;
    std::istringstream in(line);
    std::string tok;
    while (in >> tok) {
        const size_t eq = tok.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        kv[tok.substr(0, eq)] = tok.substr(eq + 1);
    }
    return kv;
}

bool to_u64(const std::string & s, uint64_t & out) {
    if (s.empty()) return false;
    char * end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

bool to_i64(const std::string & s, long long & out) {
    if (s.empty()) return false;
    char * end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

// The preamble both formats share: the shape, then one line per block with its expert bytes.
bool read_preamble_line(const std::string & line, RoutingTrace & t) {
    const auto kv = kv_tokens(line);
    uint64_t v = 0;
    if (kv.count("layer") && kv.count("expert_bytes")) {
        uint64_t il = 0;
        if (!to_u64(kv.at("layer"), il) || !to_u64(kv.at("expert_bytes"), v) || il > (1u << 20)) return false;
        if (t.expert_bytes.size() <= il) t.expert_bytes.resize(il + 1, 0);
        t.expert_bytes[il] = v;
        return true;
    }
    if (kv.count("n_layer") && to_u64(kv.at("n_layer"), v)) t.n_layer = (uint32_t) v;
    if (kv.count("n_expert") && to_u64(kv.at("n_expert"), v)) t.n_expert = (uint32_t) v;
    if (kv.count("n_expert_used") && to_u64(kv.at("n_expert_used"), v)) t.n_expert_used = (uint32_t) v;
    if (kv.count("prefill_tokens") && to_u64(kv.at("prefill_tokens"), v)) t.prefill_tokens = (uint32_t) v;
    if (kv.count("decode_tokens") && to_u64(kv.at("decode_tokens"), v)) t.decode_tokens = (uint32_t) v;
    return true;
}

bool parse_native(std::istringstream & in, RoutingTrace & t, std::string & error) {
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (!read_preamble_line(line, t)) {
                error = "unreadable preamble line: " + line;
                return false;
            }
            continue;
        }
        std::istringstream row(line);
        std::string phase;
        long long il = -1;
        if (!(row >> phase >> il) || (phase != "p" && phase != "d") || il < 0) {
            error = "unreadable batch line: " + line;
            return false;
        }
        RoutingBatch b;
        b.layer = (uint32_t) il;
        b.decode = phase == "d";
        long long e = 0;
        while (row >> e) {
            if (e < 0 || (t.n_expert && e >= (long long) t.n_expert)) {
                error = "expert id out of range: " + line;
                return false;
            }
            b.experts.push_back((int32_t) e);
        }
        t.batches.push_back(std::move(b));
    }
    return true;
}

// A --route-trace CSV, as scripts/route-replay.py rebuilds it: every turn together, the prompt as
// one batch per layer (its tokens' ids ordered by step, slot), then each decoded step's layers in
// block order.
bool parse_route_csv(std::istringstream & in, RoutingTrace & t, std::string & error) {
    using Pre = std::tuple<long long, long long, long long>; // step, slot, expert
    std::map<uint32_t, std::vector<Pre>> pre;
    std::map<std::pair<long long, uint32_t>, std::vector<std::pair<long long, long long>>> dec; // slot, expert
    std::set<uint32_t> layers;
    std::set<std::pair<long long, long long>> pre_steps, dec_steps; // (turn, step)
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (!read_preamble_line(line, t)) {
                error = "unreadable preamble line: " + line;
                return false;
            }
            continue;
        }
        if (line.compare(0, 5, "turn,") == 0) continue;
        std::vector<std::string> f;
        std::string cell;
        std::istringstream row(line);
        while (std::getline(row, cell, ','))
            f.push_back(cell);
        if (f.size() < 9) continue; // the script skips a short row too
        long long turn = 0, phase = 0, step = 0, layer = 0, slot = 0, expert = 0;
        if (!to_i64(f[0], turn) || !to_i64(f[1], phase) || !to_i64(f[2], step) || !to_i64(f[3], layer) ||
            !to_i64(f[4], slot) || !to_i64(f[5], expert) || layer < 0 || expert < 0) {
            error = "unreadable row: " + line;
            return false;
        }
        layers.insert((uint32_t) layer);
        if (phase == 0) {
            pre[(uint32_t) layer].emplace_back(step, slot, expert);
            pre_steps.insert({turn, step});
        } else {
            dec[{step, (uint32_t) layer}].emplace_back(slot, expert);
            dec_steps.insert({turn, step});
        }
    }
    for (uint32_t il : layers) {
        auto it = pre.find(il);
        if (it == pre.end()) continue;
        std::sort(it->second.begin(), it->second.end());
        RoutingBatch b;
        b.layer = il;
        for (const Pre & p : it->second)
            b.experts.push_back((int32_t) std::get<2>(p));
        t.batches.push_back(std::move(b));
    }
    std::set<long long> steps;
    for (const auto & kv : dec)
        steps.insert(kv.first.first);
    for (long long step : steps) {
        for (uint32_t il : layers) {
            auto it = dec.find({step, il});
            if (it == dec.end()) continue;
            std::sort(it->second.begin(), it->second.end());
            RoutingBatch b;
            b.layer = il;
            b.decode = true;
            for (const auto & se : it->second)
                b.experts.push_back((int32_t) se.second);
            t.batches.push_back(std::move(b));
        }
    }
    t.prefill_tokens = (uint32_t) pre_steps.size();
    t.decode_tokens = (uint32_t) dec_steps.size();
    if (t.n_layer == 0 && !layers.empty()) t.n_layer = *layers.rbegin() + 1;
    return true;
}

} // namespace

std::string format_routing_trace(const RoutingTrace & t) {
    std::string out = k_native_magic;
    out += "\n";
    char buf[192];
    std::snprintf(buf, sizeof(buf), "# n_layer=%u n_expert=%u n_expert_used=%u prefill_tokens=%u decode_tokens=%u\n",
                  t.n_layer, t.n_expert, t.n_expert_used, t.prefill_tokens, t.decode_tokens);
    out += buf;
    for (size_t il = 0; il < t.expert_bytes.size(); ++il) {
        if (t.expert_bytes[il] == 0) continue;
        std::snprintf(buf, sizeof(buf), "# layer=%zu expert_bytes=%llu\n", il, (unsigned long long) t.expert_bytes[il]);
        out += buf;
    }
    for (const RoutingBatch & b : t.batches) {
        out += b.decode ? "d " : "p ";
        out += std::to_string(b.layer);
        for (int32_t e : b.experts) {
            out += ' ';
            out += std::to_string(e);
        }
        out += '\n';
    }
    return out;
}

bool parse_routing_trace(const std::string & text, RoutingTrace & out, std::string & error) {
    RoutingTrace t;
    std::istringstream in(text);
    std::string first;
    std::getline(in, first);
    if (!first.empty() && first.back() == '\r') first.pop_back();
    bool ok = false;
    if (first == k_native_magic)
        ok = parse_native(in, t, error);
    else if (first == k_route_trace_magic)
        ok = parse_route_csv(in, t, error);
    else
        error = "not a routing trace: it starts with neither '" + std::string(k_native_magic) + "' nor '" +
                k_route_trace_magic + "'";
    if (!ok) return false;
    for (const RoutingBatch & b : t.batches) {
        if (b.layer >= t.expert_bytes.size() || t.expert_bytes[b.layer] == 0) {
            // A layer that costs nothing would be admitted free and never evicted, and the replay would
            // report hits at every size: a wrong answer that looks like a good one.
            error = "no expert bytes for layer " + std::to_string(b.layer) + " in the preamble";
            return false;
        }
    }
    out = std::move(t);
    return true;
}

// ── the replay ───────────────────────────────────────────────────────────────────────────────
ReplayCounts replay_lru(const RoutingTrace & t, uint64_t budget) {
    ReplayCounts r;
    const uint64_t n_expert = t.n_expert;
    if (n_expert == 0) return r;
    const size_t n_keys = (size_t) t.expert_bytes.size() * (size_t) n_expert;

    // LRU order front (least recent) to back; a key's node, and the batch that last touched it.
    std::list<uint32_t> order;
    std::vector<std::list<uint32_t>::iterator> node(n_keys);
    std::vector<uint8_t> resident(n_keys, 0);
    std::vector<uint64_t> stamp(n_keys, 0); // 1 + index of the batch that last touched the key
    uint64_t used = 0;

    for (size_t bi = 0; bi < t.batches.size(); ++bi) {
        const RoutingBatch & b = t.batches[bi];
        const uint64_t cost = t.expert_bytes[b.layer];
        const uint64_t mark = bi + 1;
        for (int32_t e : b.experts) {
            const uint32_t key = (uint32_t) (b.layer * n_expert + (uint64_t) e);
            if (stamp[key] == mark) { // a repeat inside the batch promotes and is not a lookup
                order.splice(order.end(), order, node[key]);
                continue;
            }
            stamp[key] = mark;
            ++r.lookups;
            if (b.decode) ++r.decode_lookups;
            if (resident[key]) {
                ++r.hits;
                if (b.decode) ++r.decode_hits;
                order.splice(order.end(), order, node[key]);
            } else {
                resident[key] = 1;
                used += cost;
                r.read_bytes += cost;
                node[key] = order.insert(order.end(), key);
            }
        }
        // The batch's own entries are the most recent ones, so the least recent entry is evictable
        // unless every resident entry belongs to this batch.
        while (used > budget && !order.empty() && stamp[order.front()] != mark) {
            const uint32_t victim = order.front();
            order.pop_front();
            resident[victim] = 0;
            used -= t.expert_bytes[victim / n_expert];
        }
    }
    return r;
}

HitCurve hit_curve(const RoutingTrace & t, uint32_t points) {
    HitCurve c;
    c.decode_tokens = t.decode_tokens;
    c.prefill_tokens = t.prefill_tokens;
    const uint64_t set = t.expert_set_bytes();
    if (set == 0 || points == 0 || t.empty()) return c;
    c.points.push_back({0, 0.0});
    double best = 0.0;
    bool decoded = false;
    for (uint32_t i = 1; i <= points; ++i) {
        const uint64_t size = (uint64_t) ((double) set * (double) i / (double) points);
        const ReplayCounts r = replay_lru(t, size);
        decoded = decoded || r.decode_lookups > 0;
        best = std::max(best, r.decode_hit());
        c.points.push_back({size, best});
    }
    c.measured = decoded;
    if (!decoded) c.points.clear();
    return c;
}

std::vector<HotExpert> hottest_experts(const RoutingTrace & t, size_t limit) {
    std::map<std::pair<uint32_t, int32_t>, uint64_t> uses;
    for (const RoutingBatch & b : t.batches)
        for (int32_t e : b.experts)
            ++uses[{b.layer, e}];
    std::vector<HotExpert> out;
    out.reserve(uses.size());
    for (const auto & kv : uses)
        out.push_back({kv.first.first, kv.first.second, kv.second});
    std::stable_sort(out.begin(), out.end(), [](const HotExpert & a, const HotExpert & b) {
        if (a.uses != b.uses) return a.uses > b.uses;
        if (a.layer != b.layer) return a.layer < b.layer;
        return a.expert < b.expert;
    });
    if (limit && out.size() > limit) out.resize(limit);
    return out;
}

void attach_routing(ModelProfile & m, const RoutingTrace & t, uint32_t n_expert_used) {
    m.hit_curve = HitCurve{};
    auto reject = [&](std::string why) { m.hit_curve.rejected = std::move(why); };
    if (t.empty()) return reject("the stored routing trace is empty");
    if (!m.ok || !m.is_moe) return reject("the model has no routed experts to price a cache for");
    if (t.n_expert != m.n_expert)
        return reject("the trace routed over " + std::to_string(t.n_expert) + " experts and this model has " +
                      std::to_string(m.n_expert));
    if (t.n_expert_used != n_expert_used)
        return reject("the trace was recorded at top-" + std::to_string(t.n_expert_used) + " and this run routes top-" +
                      std::to_string(n_expert_used));
    if (t.decode_tokens < k_routing_min_decode_tokens)
        return reject("the trace decoded " + std::to_string(t.decode_tokens) + " tokens, under the " +
                      std::to_string(k_routing_min_decode_tokens) + " a curve is read from");
    std::set<uint32_t> traced;
    for (const RoutingBatch & b : t.batches)
        traced.insert(b.layer);
    if (!m.moe_layers.empty() && traced != std::set<uint32_t>(m.moe_layers.begin(), m.moe_layers.end()))
        return reject("the trace routed through " + std::to_string(traced.size()) + " blocks and this model has " +
                      std::to_string(m.moe_layers.size()) + " carrying experts");
    const double set = (double) t.expert_set_bytes();
    const double file = (double) m.expert_bytes;
    if (file <= 0.0 || set < file * (1.0 - k_routing_bytes_tolerance) || set > file * (1.0 + k_routing_bytes_tolerance))
        return reject("the trace's expert set is " + std::to_string((unsigned long long) (set / (1024.0 * 1024.0))) +
                      " MiB and this model's is " + std::to_string((unsigned long long) (file / (1024.0 * 1024.0))) +
                      " MiB");
    m.hit_curve = hit_curve(t, k_hit_curve_points);
    if (!m.hit_curve.measured) reject("the trace has no decoded tokens");
}

} // namespace bmoe
