// probe_devices (core/src/plan/hardware_probe.cpp): which of ggml's registered devices the planner
// is told about. The rule under test is a fact, not a name: a device ggml registers as an
// accelerator (BLAS, AMX, ...) is a library the host's graphs call into and never a placement
// target, so it is left out of the profile; every other device (CPU, GPU, integrated GPU, an NPU
// that registers as a GPU) is kept. Run against this build's real registry, so on a build that
// links BLAS (macOS by default) the exclusion is exercised, and on any build the keep side is.
#include "bmoe/hardware_profile.h"
#include "bmoe/probe.h"

#include "ggml-backend.h"

#include <cstdio>
#include <string>

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
    const bmoe::HardwareProfile hw = bmoe::probe_hardware(nullptr);

    const auto listed = [&](const std::string & name) {
        for (const bmoe::ComputeDevice & d : hw.devices)
            if (d.name == name) return true;
        return false;
    };

    int n_accel = 0, n_kept = 0;
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        const std::string name = ggml_backend_dev_name(dev);
        if (ggml_backend_dev_type(dev) == GGML_BACKEND_DEVICE_TYPE_ACCEL) {
            ++n_accel;
            CHECK(!listed(name), ("an accelerator is not a device: " + name).c_str());
        } else {
            ++n_kept;
            CHECK(listed(name), ("a placeable device is listed: " + name).c_str());
        }
    }
    CHECK(hw.devices.size() == (size_t) n_kept, "the profile lists exactly the placeable devices");
    bool has_cpu = false;
    for (const bmoe::ComputeDevice & d : hw.devices)
        has_cpu = has_cpu || d.is_cpu;
    CHECK(has_cpu, "the host is always listed");

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("device_probe: ok (%d listed, %d accelerator(s) left out)\n", n_kept, n_accel);
    return 0;
}
