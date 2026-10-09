// Adapters that fill the planner's two inputs from the real world.
//
// This is the layer where platform names are allowed, and the only one. Everything above it —
// HardwareProfile, ModelProfile, the rules — is written in terms of facts, so a new platform is a
// new branch HERE and nothing else changes. Keeping that boundary is what makes the planner
// testable against machines nobody here owns.
#pragma once

#include "bmoe/hardware_profile.h"
#include "bmoe/model_profile.h"
#include "bmoe/placement.h"

#include <functional>
#include <string>
#include <vector>

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

// Ask every registered device whether it can execute THIS model's expert matmul on THIS model's
// quantized layout, and whether placing such a weight there would need a repack. Asking the backend
// about the real operation is what keeps the answer vendor-neutral and current: a backend that
// gains a kernel starts answering yes with nothing here changing, and one that only executes a
// repacked layout is excluded from the streamed experts for a stated reason rather than by name.
//
// Fills the per-device fields of `hw`, and leaves them Unknown where the question could not be
// asked (no devices registered yet, or an unknown expert type) — which every rule declines on.
void probe_device_support(HardwareProfile & hw, const ModelProfile & model);

// Make every backend this machine has available for enumeration, including any that ship as
// separate shared libraries rather than linked in. llama.cpp's own tools do this before they look
// at devices, and the reason to do it here is the same: a device nobody enumerated is a device no
// rule can consider, and "there is no accelerator" and "nobody went to look" are different answers.
//
// A statically linked build finds nothing and loses nothing. Call it before llama_backend_init().
void register_backends();

// What this binary can even see. A backend the build did not compile in, and did not find beside
// the executable, is a device no probe will ever enumerate — so a plan made on this machine would
// optimise for a machine with no accelerator and never say why. The fields keep the three answers
// apart: how many came linked, how many were loaded at run time, and whether anyone looked at all.
// Only meaningful after register_backends().
struct BackendInventory {
    uint32_t linked = 0;            // compiled into this binary
    uint32_t loaded_at_runtime = 0; // found as separate shared libraries beside the executable
    bool looked = false;            // register_backends() ran; false means nobody went to look
    std::vector<std::string> names; // every registered backend, for the rationale and the CSV header

    uint32_t total() const { return linked + loaded_at_runtime; }
};

BackendInventory backend_inventory();

// What a device COSTS to use, settled by using it: whether its memory is really the host's, and
// what one host/device boundary crossing costs a token. Both are the kind of fact a declaration
// gets wrong - a device type is not a memory topology, and a bandwidth figure cannot see a
// crossing - and both fail closed, leaving the fact Unknown or zero where the measurement could
// not be made. Needs the backends registered; run it on a quiet machine, before the probes that
// dirty gigabytes. See core/src/plan/device_probe.cpp.
void probe_device_costs(HardwareProfile & hw, const ModelProfile & model);

// What each compute engine can pull out of the memory it reads weights from, in GiB/s. One graph -
// the same GEMV - scheduled on every backend, so the figures are comparable: the rules only ever use
// the ratio of two of them, and a ratio between two different experiments would mean nothing. The
// CPU backend's result is also the host's figure. Devices are only registered after
// llama_backend_init(), and a device that will not allocate or has no kernel for the op keeps its
// unmeasured 0, which every rule declines on rather than reading as "slow".
// `model` supplies the quantized type the weights actually are. That matters more than it looks:
// an F32 GEMV saturates a memory bus at very few threads, while a quantized one carries
// dequantisation work per byte and keeps scaling past that point. Measured with F32 on a phone, the
// sweep answered 2 threads - the same wrong answer as the rule it replaced, and the engine is 58%
// faster at 4. The instrument has to be made of the same material as the workload.
void probe_bandwidth(HardwareProfile & hw, const ModelProfile & model);

// ── probing a device where it cannot take the plan down with it ──────────────────────────────
//
// Every probe above that touches a device does so by using it, and a device is only as sound as
// its driver. Seen on a phone whose graphics driver predates what a backend assumes: the backend
// loads, lists the device, and the first allocation calls a function the driver does not have -
// the process is gone, with no error to catch. A probe in this process cannot survive that, so the
// plan that was meant to find the right hardware instead stops the program on the wrong one.
//
// So a device can be probed in ANOTHER process. `probe_one_device` is that process's whole job:
// everything above that touches `device`, and its facts as one line of text. `probe_devices_isolated`
// is the parent's side: for each device it asks a runner for that line, takes the facts if it comes
// back, and otherwise marks the device unusable with the reason - after which no probe here and no
// rule above will touch it. How a second process is started is the front-end's business, which is
// why the runner is a parameter; `self_process_runner` is the common answer, this same executable.
//
// Where no runner is given the probes run here as before: correct on every device that works,
// and exactly as exposed as it always was to one that does not.
using DeviceProbeRunner = std::function<bool(const std::string & device, std::string & facts, std::string & why)>;

void probe_devices_isolated(HardwareProfile & hw, const DeviceProbeRunner & run);

// The child's side. Backends must be registered and llama_backend_init() called first, as for the
// probes it runs. False when the device is not there to probe.
bool probe_one_device(const char * model_path, const std::string & device, std::string & facts);

// A device's measured facts as one line of text, and back. The name is not carried: the caller
// knows which device it asked about. `from_text` leaves the device untouched and returns false on
// anything it does not recognise as a complete line.
std::string device_facts_to_text(const ComputeDevice & d);
bool device_facts_from_text(const std::string & text, ComputeDevice & d);
// The marker a child prints its facts line after, so a parent can find it among a backend's own
// output on the same stream.
extern const char * const device_facts_marker;

// A runner that starts this same executable again with `argv_for(device)` and gives it
// `timeout_seconds` to answer. Fails - with the reason - when the child exits abnormally, runs out
// of time or prints no facts; and, where this platform has no way to do it, returns a runner that
// is empty (`!runner`), which a caller reads as "probe here instead".
DeviceProbeRunner self_process_runner(std::function<std::vector<std::string>(const std::string & device)> argv_for,
                                      double timeout_seconds);

// How much memory this process can hold and expect to keep, which is not what the machine reports
// as available: where a reclaim compresses, that figure is a floor for what could be taken and an
// over-promise for what could be kept. Fills hw.holdable_bytes, or leaves it 0 where this machine
// will not say - every rule then falls back to the reported budget and states which one it used.
//
// `allow_active` opts into the measurement rather than the estimate: holding memory in steps until
// the kernel takes some back. It is intrusive by nature - seconds of wall clock and real pressure
// on the machine - so it is off unless a caller asks for it, while the estimate costs nothing.
// `target_bytes` is what the caller actually wants to hold - on this engine, the dense set plus one
// token's worth of experts, the smallest configuration worth running. The active probe stops as soon
// as it has held that much, and never probes far past it. That bound is not politeness: a probe that
// climbs towards the machine's total size PROVOKES the reclaim it is trying to observe, and then
// reports the pressure it caused. Measured on a phone that comfortably holds 4.4 GB in practice, the
// unbounded version answered 924 MiB.
void probe_headroom(HardwareProfile & hw, bool allow_active, uint64_t target_bytes);

// Measure this storage with the reads this engine issues: the rate curve around `slice_bytes` for
// one, two and four lanes, and whether a live mapping of the model serialises those reads. Costs
// real I/O — a few tens of MiB and well under a second — which is why it is separate from the free
// facts above and runs only when asked. Fills hw.storage; leaves what it could not measure Unknown.
void probe_storage(HardwareProfile & hw, const char * model_path, uint64_t slice_bytes);

// The first stage: run llama.cpp's own capacity fitter on this model and read off what the second
// stage needs — which layers kept their experts on the host, and how much host memory the placed
// model takes. `n_ctx` 0 lets the fitter choose (it shrinks context before moving weights); a set
// value is a pin. Returns fitted=false where the fitter could not run, and the planner then plans
// as if there were no devices at all.
Placement
probe_placement(const char * model_path, const ModelProfile & model, const HardwareProfile & hw, uint32_t n_ctx);

} // namespace bmoe
