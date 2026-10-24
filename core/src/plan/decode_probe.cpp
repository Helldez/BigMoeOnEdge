// This model's own decode, timed on one engine (bmoe/probe.h, probe_decode).
//
// Asked only where a whole-model move is on the table: a model resident in a pool a device shares,
// so the run would hold every weight in memory either way and loading it twice reads nothing new
// from storage. Every other question the planner asks of a device is answered by a part of the
// workload; this one is not, because nothing smaller predicted it (see ComputeDevice::decode_tps).

#include "bmoe/probe.h"

#include "llama.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace bmoe {

namespace {

// Long enough that the steady state dominates the first steps (kernel selection, first touches of
// pages), short enough that measuring two engines costs seconds rather than a benchmark. A context
// of this size keeps the KV and compute buffers small beside the weights the run holds anyway.
constexpr int k_warmup_tokens = 4;
constexpr int k_timed_tokens = 16;
constexpr uint32_t k_ctx = 256;

} // namespace

double probe_decode(const char * model_path, const std::string & device, int n_threads) {
    llama_model_params mp = llama_model_default_params();
    mp.load_mode = LLAMA_LOAD_MODE_MMAP; // as the run loads it: hard rule 2, and the pages already in memory
    mp.use_extra_bufts = false;
    ggml_backend_dev_t devs[2] = {nullptr, nullptr};
    if (!device.empty()) {
        devs[0] = ggml_backend_dev_by_name(device.c_str());
        if (!devs[0]) return 0.0;
        mp.n_gpu_layers = -1; // every layer
    } else {
        mp.n_gpu_layers = 0;
    }
    mp.devices = devs; // the named device alone, or none: the CPU is added by llama.cpp either way

    llama_model * model = nullptr;
    try {
        model = llama_model_load_from_file(model_path, mp);
    } catch (const std::exception &) {
        model = nullptr;
    }
    if (!model) return 0.0;

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = k_ctx;
    cp.n_batch = 1;
    cp.n_ubatch = 1;
    if (n_threads > 0) cp.n_threads = cp.n_threads_batch = n_threads;
    llama_context * ctx = nullptr;
    try {
        ctx = llama_init_from_model(model, cp);
    } catch (const std::exception &) {
        ctx = nullptr;
    }
    double tps = 0.0;
    if (ctx) {
        // The token fed is immaterial to the time a step takes; any id the vocabulary has will do.
        const llama_vocab * vocab = llama_model_get_vocab(model);
        llama_token tok = llama_vocab_bos(vocab);
        if (tok < 0) tok = 0;
        bool ok = true;
        double secs = 0.0;
        for (int i = 0; ok && i < k_warmup_tokens + k_timed_tokens; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            ok = llama_decode(ctx, llama_batch_get_one(&tok, 1)) == 0;
            llama_synchronize(ctx);
            if (i >= k_warmup_tokens)
                secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        if (ok && secs > 0.0) tps = k_timed_tokens / secs;
        llama_free(ctx);
    }
    llama_model_free(model);
    return tps;
}

bool probe_decode_child(const char * model_path, const std::string & device, int n_threads, std::string & line) {
    llama_backend_init(); // a child starts bare; the backends are the front-end's to register first
    const double tps = probe_decode(model_path, device, n_threads);
    if (tps <= 0.0) return false;
    char b[64];
    std::snprintf(b, sizeof(b), " decode=%.6g", tps);
    line = std::string(device_facts_marker) + b;
    return true;
}

void probe_decode_rates(HardwareProfile & hw,
                        const char * model_path,
                        const std::vector<std::string> & devices,
                        const DeviceProbeRunner & run) {
    hw.host_decode_tps = probe_decode(model_path, std::string(), (int) hw.best_threads);
    for (const std::string & name : devices) {
        for (ComputeDevice & d : hw.devices) {
            if (d.is_cpu || d.name != name) continue;
            if (!run) {
                d.decode_tps = probe_decode(model_path, name, (int) hw.best_threads);
                break;
            }
            // In a process of its own, for the reason every device probe is (probe_devices_isolated):
            // a driver that ends the process it is used from ends the child, and the device is then
            // marked as having no measured decode rather than ending the plan.
            std::string facts, why;
            if (run(name, facts, why)) {
                const size_t at = facts.find("decode=");
                if (at != std::string::npos) d.decode_tps = std::strtod(facts.c_str() + at + 7, nullptr);
            }
            break;
        }
    }
}

} // namespace bmoe
