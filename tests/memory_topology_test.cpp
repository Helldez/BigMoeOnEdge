#include "memory_topology.h"

#include "bmoe/probe.h"

#include <cstdio>

using bmoe::Tri;
using bmoe::detail::refine_memory_sharing;

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char * name) {
        std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) ++failures;
    };

    check(refine_memory_sharing(Tri::Yes, Tri::No) == Tri::Yes,
          "a flat allocation delta cannot turn a known shared pool into extra capacity");
    check(refine_memory_sharing(Tri::Yes, Tri::Unknown) == Tri::Yes,
          "an unavailable sample preserves known shared memory");
    check(refine_memory_sharing(Tri::Unknown, Tri::Yes) == Tri::Yes,
          "a positive allocation charge establishes shared memory");
    check(refine_memory_sharing(Tri::No, Tri::Yes) == Tri::Yes,
          "a host charge takes precedence over an earlier separate-memory conclusion");
    check(refine_memory_sharing(Tri::No, Tri::Unknown) == Tri::No,
          "an unavailable sample preserves a known separate pool");
    check(refine_memory_sharing(Tri::Unknown, Tri::No) == Tri::No,
          "the allocation probe retains its existing separate-memory path");

#if defined(BMOE_HAVE_METAL_MEMORY_PROBE)
    check(bmoe::detail::metal_memory_sharing(nullptr) == Tri::Unknown, "no device description means unknown");
    check(bmoe::detail::metal_memory_sharing("not a native Metal device") == Tri::Unknown,
          "a description mismatch cannot borrow another device's topology");
    bmoe::register_backends();
    const bmoe::HardwareProfile hw = bmoe::probe_hardware(nullptr);
    for (const auto & device : hw.devices) {
        if (device.name != "MTL0") continue;
        const Tri native = bmoe::detail::metal_memory_sharing(device.description.c_str());
        if (native == Tri::Unknown) {
            std::puts("[SKIP] native Metal topology unavailable or description does not match");
            continue;
        }
        check(device.shares_host_memory == native, "the hardware probe carries native Metal topology into the profile");
        bmoe::ComputeDevice roundtrip = device;
        check(bmoe::device_facts_from_text(bmoe::device_facts_to_text(device), roundtrip) &&
                  roundtrip.shares_host_memory == native,
              "the isolated-probe protocol preserves native topology");
    }
#endif
    return failures ? 1 : 0;
}
