// Where a record was made and what the system's memory did around a run: the two things a plan
// record carries that no rule reads (plan_record.h, PlanRecord::env and ::observed).
#pragma once

#include "plan_record.h"

namespace bmoe {

// The operating system and its version, the kernel, the architecture and the processor's name, how
// much swap there is and of what kind, and what else decides how memory behaves there (a
// container's limit, the swappiness). Whatever the platform publishes; a key it does not is absent.
void probe_environment(Facts & env);

// The system's memory counters now: swap in use, pages swapped in and out since boot, compressions,
// major faults, pressure. Taken before and after a run, the difference is what the run did to the
// machine - and what the machine did to the run.
void sample_system(Facts & out);

} // namespace bmoe
