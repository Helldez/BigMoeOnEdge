#include "sim_speed.h"

#include "bmoe/allocate.h"

#include <algorithm>
#include <cmath>
#include <regex>
#include <set>

namespace bmoe {

namespace {

constexpr double k_gib = 1024.0 * 1024.0 * 1024.0;

double seconds(double bytes, double gibs) {
    return gibs > 0.0 ? bytes / k_gib / gibs : NAN;
}

// The layers whose routed experts a buffer-type override keeps on the host: the fitter writes one
// pattern per layer, `blk\.N\.` and the expert tensors' names (tensor_names.h is the engine's parser;
// the tool reads only the index the fitter put in front).
std::set<uint32_t> host_expert_layers(const std::vector<std::string> & overrides) {
    static const std::regex blk(R"(blk\\+\.(\d+)\\+\.)");
    std::set<uint32_t> out;
    for (const std::string & p : overrides) {
        if (p.find("exps") == std::string::npos) continue;
        std::smatch m;
        if (std::regex_search(p, m, blk)) out.insert((uint32_t) std::stoul(m[1].str()));
    }
    return out;
}

} // namespace

DecodeEstimate estimate_decode(const Plan & plan,
                               const HardwareProfile & hw,
                               const ModelProfile & model,
                               const Placement & placement) {
    DecodeEstimate e;
    const RunConfig & c = plan.config;
    const uint32_t L = model.n_layer;
    const uint32_t M = model.n_moe_layer ? model.n_moe_layer : L;
    if (L == 0) {
        e.why = "the model's layer count is unknown";
        return e;
    }
    auto part = [&e](const std::string & name, double s) {
        if (std::isnan(s)) {
            e.why = "the rate for " + name + " is unmeasured";
            return;
        }
        if (s > 0.0) e.parts.emplace_back(name, s);
    };

    // The cards that hold layers, with how many each: the plan's split, else all on the first card.
    std::vector<std::pair<const ComputeDevice *, int>> cards;
    const int on_cards = c.n_gpu_layers < 0 ? (int) L + 1 : std::min(c.n_gpu_layers, (int) L + 1);
    for (const auto & [name, n] : c.device_split)
        for (const ComputeDevice & d : hw.devices)
            if (d.name == name && !d.is_cpu) cards.emplace_back(&d, n);
    if (cards.empty() && on_cards > 0)
        for (const ComputeDevice & d : hw.devices)
            if (!d.is_cpu && d.usable != Tri::No) {
                cards.emplace_back(&d, on_cards);
                break;
            }
    const bool shared_pool = !cards.empty() && !cards.front().first->has_own_memory();
    const int card_layers = cards.empty() || shared_pool ? 0 : std::min(on_cards, (int) L);

    // A token's bytes by group.
    const double dense_tok = (double) model.group(WeightGroup::Attention).bytes_per_token +
                             (double) model.group(WeightGroup::DenseFfn).bytes_per_token +
                             (double) model.group(WeightGroup::Other).bytes_per_token;
    const double output_tok = (double) model.group(WeightGroup::Output).bytes_per_token;
    const double experts_tok = (double) model.group(WeightGroup::Experts).bytes_per_token;

    // ── the dense part ──
    const double host_dense = hw.host_dense_rate();
    for (const auto & [d, n] : cards)
        if (!shared_pool)
            part("dense on " + d->name, seconds(dense_tok * std::min(n, (int) L) / L, dense_rates(hw, *d).device));
    part("dense on the host", seconds(dense_tok * (double) (L - card_layers) / L, host_dense));
    const bool output_on_card = !shared_pool && on_cards > (int) L && !cards.empty();
    part("output", seconds(output_tok, output_on_card ? dense_rates(hw, *cards.back().first).device : host_dense));

    // ── the experts ──
    std::set<uint32_t> host_layers = host_expert_layers(c.buft_overrides);
    for (uint32_t i = (uint32_t) card_layers; i < L; ++i)
        host_layers.insert(i);
    const double host_share = std::min(1.0, (double) host_layers.size() / (double) M);
    const ComputeDevice * slowest = nullptr;
    for (const auto & [d, n] : cards)
        if (!slowest || expert_rates(hw, *d).device < expert_rates(hw, *slowest).device) slowest = d;
    if (slowest && !shared_pool && host_share < 1.0)
        part("experts on the cards", seconds(experts_tok * (1.0 - host_share), expert_rates(hw, *slowest).device));

    const double host_experts_tok = experts_tok * host_share;
    const uint64_t dcache = (uint64_t) std::max(0, c.moe.device_cache_mb) << 20;
    if (host_experts_tok > 0.0 && dcache > 0 && slowest && !shared_pool) {
        // The cache's share of the host's experts: the model's own routing replayed at its size where a
        // record of it was given, else the fraction of those experts it holds.
        const uint64_t host_expert_bytes = (uint64_t) ((double) model.expert_bytes * host_share);
        double hit = 0.0;
        if (model.hit_curve.measured) {
            hit = model.hit_curve.hit_share((uint64_t) ((double) dcache / std::max(host_share, 1e-9)));
            e.hit_from_routing = true;
        } else if (host_expert_bytes > 0) {
            hit = std::min(1.0, (double) dcache / (double) host_expert_bytes);
        }
        e.device_cache_hit = hit;
        const double dev = expert_rates(hw, *slowest).device;
        const double up = !c.moe.expert_host_buffer.empty() && slowest->upload_host_buffer_gibs > 0.0
                              ? slowest->upload_host_buffer_gibs
                              : slowest->upload_gibs;
        const double miss_tok = host_experts_tok * (1.0 - hit);
        part("device cache hits", seconds(host_experts_tok * hit, dev));
        const double share = c.moe.device_cache_cpu_share;
        if (share > 0.0) {
            const double on_card = seconds(miss_tok * (1.0 - share), up) + seconds(miss_tok * (1.0 - share), dev);
            const double on_host = seconds(miss_tok * share, hw.host_expert_rate());
            part("device cache misses (card and host side by side)", std::max(on_card, on_host));
            part("host hops", (double) M * slowest->host_hop_seconds);
        } else {
            part("device cache misses uploaded", seconds(miss_tok, up));
            part("device cache misses computed", seconds(miss_tok, dev));
        }
    } else if (host_experts_tok > 0.0) {
        if (c.moe.enabled) {
            // Streamed: the cost model's own price of the host's experts (RAM cache hits, flash reads).
            part("experts streamed (the cost model's price)", plan.allocation.at(WeightGroup::Experts).seconds_per_token);
        } else {
            part("experts on the host", seconds(host_experts_tok, hw.host_expert_rate()));
        }
        // A card layer whose experts the host computes crosses to the host and back.
        if (slowest && !shared_pool) {
            uint32_t crossing_layers = 0;
            for (uint32_t i : host_layers)
                if ((int) i < card_layers) ++crossing_layers;
            part("crossings", 2.0 * crossing_layers * slowest->split_seconds);
        }
    }

    if (!e.why.empty()) {
        e.parts.clear();
        return e;
    }
    for (const auto & p : e.parts)
        e.seconds += p.second;
    (void) placement;
    return e;
}

} // namespace bmoe
