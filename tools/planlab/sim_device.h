// A compute device that is not there: a ggml backend device that reports the memory a spec gives
// it and hands everything else to the CPU. Registered before a plan, it lets llama.cpp's own
// capacity fitter place a real model file on a card this machine does not have - two 16 GB cards,
// an 8 GB laptop part - so the layers, the experts left on the host, the KV and the compute buffers
// it projects are the fitter's own, not a second model of it (docs/hardware-simulation.md).
//
// What it imitates of a discrete card is what the fitter's arithmetic reads: its own memory (not
// the host's), the buffer alignment and the row padding a CUDA buffer gives a quantized tensor, and
// the batch size from which a weight on the host is computed on the device instead (op offload).
// It never computes: a plan never runs a graph on it.
//
// Built only into bmoe-planlab. It reaches into ggml's backend interface (ggml-backend-impl.h),
// which is not llama.cpp's public API: that is acceptable for a developer's instrument and for
// nothing a user runs. A llama.cpp bump that moves the interface breaks this file, not the engine.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bmoe {

struct SimDeviceSpec {
    std::string description;  // what the device reports as its description ("RTX A4000 class")
    uint64_t memory_total = 0; // bytes
    uint64_t memory_free = 0;  // bytes; 0: the total less what a driver keeps (sim_driver_reserve)
};

// What a driver and a desktop keep of a discrete card before any process asks: the difference
// between memory_total and memory_free that the rented cards' records show (a few hundred MiB).
constexpr uint64_t sim_driver_reserve_bytes = 300ull << 20;

// Register the devices, named SIM0, SIM1, ... in the order given. Once per process; a second call
// returns the names of the first.
std::vector<std::string> register_sim_devices(const std::vector<SimDeviceSpec> & specs);

} // namespace bmoe
