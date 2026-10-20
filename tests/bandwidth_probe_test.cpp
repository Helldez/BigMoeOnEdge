// probe_bandwidth (core/src/plan/bandwidth_probe.cpp) on every expert format ggml cannot encode
// without importance weights. The probe quantizes synthetic bytes into the model's own expert
// type; for those formats ggml asserts on a missing imatrix, and `--auto` died on a UD-IQ2 model
// before it loaded anything (#321). Which formats they are is asked of ggml, so a format that
// joins the list is covered without this file changing. Host only: devices are not the question.
#include "bmoe/hardware_profile.h"
#include "bmoe/model_profile.h"
#include "bmoe/probe.h"

#include "ggml.h"

#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond, what)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what);                                        \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

int main() {
    bmoe::register_backends();
    bmoe::HardwareProfile base = bmoe::probe_hardware(nullptr);
    std::vector<bmoe::ComputeDevice> host;
    for (const bmoe::ComputeDevice & d : base.devices)
        if (d.is_cpu) host.push_back(d);
    CHECK(!host.empty(), "the host is listed");
    base.devices = host;

    int n = 0;
    for (int t = 0; t < GGML_TYPE_COUNT; ++t) {
        const ggml_type type = (ggml_type) t;
        if (ggml_blck_size(type) <= 0 || !ggml_quantize_requires_imatrix(type)) continue;
        ++n;
        bmoe::HardwareProfile hw = base;
        bmoe::ModelProfile model;
        model.expert_type_id = t;
        model.expert_type_ids = {t};
        bmoe::probe_bandwidth(hw, model); // aborted here before the fix
        CHECK(hw.host_bandwidth_gibs > 0.0, ggml_type_name(type));
    }
    CHECK(n > 0, "ggml names at least one format that needs importance weights");

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("bandwidth_probe: ok (%d format(s) that need importance weights)\n", n);
    return 0;
}
