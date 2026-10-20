#pragma once

#include "bmoe/hardware_profile.h"

namespace bmoe::detail {

// A flat allocation delta can miss shared pages already charged to the process. It cannot
// contradict positive evidence that the device uses the host's physical pool.
inline Tri refine_memory_sharing(Tri known, Tri measured) {
    if (known == Tri::Yes || measured == Tri::Unknown) return known;
    return measured;
}

// The pinned Metal backend uses the system default physical device. A description mismatch
// leaves topology unknown rather than assigning another device's memory to this one.
Tri metal_memory_sharing(const char * description);

} // namespace bmoe::detail
