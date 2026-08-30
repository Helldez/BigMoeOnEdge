// Adapters that fill the planner's two inputs from the real world.
//
// This is the layer where platform names are allowed, and the only one. Everything above it —
// HardwareProfile, ModelProfile, the rules — is written in terms of facts, so a new platform is a
// new branch HERE and nothing else changes. Keeping that boundary is what makes the planner
// testable against machines nobody here owns.
#pragma once

#include "bmoe/hardware_profile.h"
#include "bmoe/model_profile.h"

namespace bmoe {

// Everything this machine will tell us for free: memory accounting, what a reclaim costs, whether a
// reclaim-exempt allocation exists, the registered compute devices, and whether uncached reads are
// in effect for `model_path` (passing nullptr skips the storage half).
//
// Registered devices are only visible after llama_backend_init(), so a caller that wants the device
// list must probe after it; the rest of the profile does not care.
//
// What it deliberately does NOT do is measure: the read-rate curve and whether a live mapping
// serialises reads both cost real I/O and belong to a separate, opt-in probe. They are left Unknown
// here, and the plan says so rather than assuming a value.
HardwareProfile probe_hardware(const char * model_path);

// The model, from its gguf metadata and tensor shapes alone: no tensor data is read and the model
// is never loaded. Returns ok=false when the file cannot be parsed, which every planning rule then
// declines on.
ModelProfile probe_model(const char * model_path);

} // namespace bmoe
