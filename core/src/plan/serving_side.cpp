#include "bmoe/serving_side.h"

#include <algorithm>

namespace bmoe {

SideWeighing weigh_sides(const SideRates & device, const SideRates & cores, double prompt_per_token, double margin) {
    SideWeighing w;
    if (!(device.decode_tps > 0.0) || !(cores.decode_tps > 0.0)) return w;
    const bool prompts = prompt_per_token > 0.0 && device.prompt_tps > 0.0 && cores.prompt_tps > 0.0;
    const auto request_s = [&](const SideRates & r) {
        return 1.0 / r.decode_tps + (prompts ? prompt_per_token / r.prompt_tps : 0.0);
    };
    w.weighed = true;
    w.device_s = request_s(device);
    w.cores_s = request_s(cores);
    w.cores_sooner = w.device_s > w.cores_s * (1.0 + std::max(margin, 0.0));
    return w;
}

} // namespace bmoe
