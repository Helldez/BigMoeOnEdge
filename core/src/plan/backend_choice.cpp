#include "bmoe/backend_choice.h"

#include <algorithm>
#include <cstdio>

namespace bmoe {

BackendChoice choose_backends(const HardwareProfile & hw, double min_win) {
    BackendChoice c;
    std::vector<const ComputeDevice *> listed;
    for (const ComputeDevice & d : hw.devices)
        if (!d.is_cpu && d.usable != Tri::No) listed.push_back(&d);
    std::vector<bool> done(listed.size(), false);
    for (size_t i = 0; i < listed.size(); ++i) {
        if (done[i] || listed[i]->physical_id.empty()) continue;
        std::vector<size_t> card = {i};
        for (size_t j = i + 1; j < listed.size(); ++j)
            if (!done[j] && listed[j]->physical_id == listed[i]->physical_id) card.push_back(j);
        for (size_t k : card)
            done[k] = true;
        if (card.size() < 2) continue;
        // One kind of figure for every backend of the card, or no comparison.
        bool real = true, gemv = true;
        for (size_t k : card) {
            real = real && listed[k]->expert_gibs > 0.0;
            gemv = gemv && listed[k]->memory_bandwidth_gibs > 0.0;
        }
        if (!real && !gemv) continue;
        const auto rate = [&](size_t k) { return real ? listed[k]->expert_gibs : listed[k]->memory_bandwidth_gibs; };
        size_t best = card.front(); // the one llama.cpp would keep
        for (size_t k : card)
            if (rate(k) > rate(best) * (1.0 + min_win)) best = k;
        if (best == card.front()) continue;
        std::string line = listed[best]->name + " drives " + listed[i]->physical_id + " at ";
        char num[64];
        std::snprintf(num, sizeof(num), "%.0f GiB/s", rate(best));
        line += num;
        for (size_t k : card) {
            if (k == best) continue;
            c.left_out.push_back(listed[k]->name);
            std::snprintf(num, sizeof(num), "%.0f", rate(k));
            line += std::string(", ") + listed[k]->name + " at " + num;
        }
        line += real ? " (this model's own expert block)" : " (a matmul in this model's format)";
        c.why.push_back(line);
    }
    if (c.left_out.empty()) return c;
    for (const ComputeDevice * d : listed)
        if (std::find(c.left_out.begin(), c.left_out.end(), d->name) == c.left_out.end()) c.keep.push_back(d->name);
    return c;
}

HardwareProfile without_left_out(const HardwareProfile & hw, const BackendChoice & choice) {
    HardwareProfile out = hw;
    out.devices.erase(std::remove_if(out.devices.begin(), out.devices.end(),
                                     [&](const ComputeDevice & d) {
                                         return std::find(choice.left_out.begin(), choice.left_out.end(), d.name) !=
                                                choice.left_out.end();
                                     }),
                      out.devices.end());
    return out;
}

} // namespace bmoe
