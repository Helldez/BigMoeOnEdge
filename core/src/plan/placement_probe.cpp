// The first stage, as an adapter: ask llama.cpp's own capacity fitter what it would do with this
// model on this machine, and read off what the second stage needs from the answer.
//
// This is the one place the engine touches `common/` (llama.cpp's helper library, which upstream
// does not treat as stable API). It is kept behind a single function returning a pure-policy
// struct so that, when the fitter's signature moves under a submodule bump, the blast radius is
// this file and the planner keeps working with `fitted = false` — declining the device axis rather
// than failing to build.

#include "bmoe/probe.h"

#include "fit.h"
#include "ggml-backend.h"
#include "llama.h"

#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace bmoe {

Placement probe_placement(const char * model_path, const ModelProfile & model, uint32_t n_ctx) {
    Placement pl;
    if (!model_path || !*model_path || !model.ok) return pl;

    // The fitter only touches parameters still at their default, and it refuses to run at all if
    // tensor_buft_overrides is already set — so the experts-on-host pin is not something we pass
    // IN; it is what we read OUT of the overrides it writes, and what the session enforces after.
    llama_model_params mparams = llama_model_default_params();
    llama_context_params cparams = llama_context_default_params();
    // The context is always pinned to the caller's. Left at 0 the fitter picks the model's full
    // training context, because it assumes system memory is unlimited - and with no device to fit
    // into, the KV and compute for that context land on the host, the one place that is not.
    cparams.n_ctx = n_ctx ? n_ctx : llama_context_default_params().n_ctx;

    std::vector<float> tensor_split(llama_max_devices(), 0.0f);
    std::vector<llama_model_tensor_buft_override> overrides(llama_max_tensor_buft_overrides());
    for (auto & o : overrides) {
        o.pattern = nullptr;
        o.buft = nullptr;
    }
    // Per-device margins, in bytes: leave a little on each device. The value is upstream's own
    // default (1 GiB) rather than a policy of ours; the second stage applies its own margin to the
    // host budget separately, from what a reclaim costs there.
    std::vector<size_t> margins(llama_max_devices() + 1, (size_t) 1024 * 1024 * 1024);

    const common_params_fit_status st =
        common_fit_params(model_path, &mparams, &cparams, tensor_split.data(), overrides.data(), margins.data(),
                          /*n_ctx_min=*/4096, /*extra=*/nullptr, GGML_LOG_LEVEL_WARN);
    if (st == COMMON_PARAMS_FIT_STATUS_ERROR) {
        pl.outcome = "the capacity fitter hit an error; planning as if there were no devices";
        return pl;
    }
    pl.fitted = true;
    pl.outcome = st == COMMON_PARAMS_FIT_STATUS_SUCCESS
                     ? "the capacity fitter found a placement that is projected to fit"
                     : "the capacity fitter could not make the model fit the devices";
    pl.n_ctx = cparams.n_ctx;

    // With no device that has memory of its own, "layers on devices" means nothing: the fitter
    // leaves n_gpu_layers at its default (all), and taken literally that would read as every
    // expert placed off-host. Count the devices rather than trusting the number.
    size_t n_dev_local = 0;
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        const enum ggml_backend_dev_type t = ggml_backend_dev_type(ggml_backend_dev_get(i));
        if (t == GGML_BACKEND_DEVICE_TYPE_GPU) ++n_dev_local;
    }
    pl.n_gpu_layers = n_dev_local ? mparams.n_gpu_layers : 0;

    // Which layers keep their routed experts on the host. Two sources, and both are read rather
    // than reconstructed: a layer beyond what the fitter placed on devices is entirely host; a
    // placed layer whose override routes its `exps` tensors to the CPU buffer type is host for the
    // experts only. Anything else went to a device and is not the streamer's to serve.
    const ggml_backend_buffer_type_t cpu_buft = ggml_backend_cpu_buffer_type();
    const uint32_t n_layer = model.n_layer;
    const uint32_t on_devices = pl.n_gpu_layers < 0 ? n_layer : std::min<uint32_t>((uint32_t) pl.n_gpu_layers, n_layer);
    const uint32_t first_device_layer = n_layer - on_devices; // llama.cpp fills devices from the top

    std::vector<bool> host(n_layer, false);
    for (uint32_t il = 0; il < first_device_layer; ++il)
        host[il] = true;

    const std::regex exps_re(R"(blk\\\.(\d+)\\\.ffn_\(up\|down\|gate_up\|gate\)_\(ch\|\)exps)");
    for (const auto & o : overrides) {
        if (!o.pattern) break;
        pl.override_patterns.push_back(o.pattern);
        std::cmatch m;
        if (o.buft == cpu_buft && std::regex_search(o.pattern, m, exps_re)) {
            const uint32_t il = (uint32_t) std::stoul(m[1].str());
            if (il < n_layer) host[il] = true;
        }
    }
    for (uint32_t il = 0; il < n_layer; ++il)
        if (host[il]) pl.host_expert_layers.push_back(il);

    // The host memory the placed model is projected to take. The breakdown's last entry is the
    // CPU; its `model` term still counts the experts that stayed on the host, which the streamer
    // takes off the books — so those are subtracted, pro rata by layer.
    std::vector<ggml_backend_dev_t> devs;
    uint32_t hp_ngl = 0, hp_nct = 0, hp_nex = 0;
    const common_device_memory_data_vec dm = common_get_device_memory_data(model_path, &mparams, &cparams, devs, hp_ngl,
                                                                           hp_nct, hp_nex, GGML_LOG_LEVEL_WARN);
    if (!dm.empty()) {
        const common_device_memory_data & cpu = dm.back();
        pl.raw_host_model_bytes = (uint64_t) cpu.model;
        pl.raw_host_context_bytes = (uint64_t) cpu.context;
        pl.raw_host_compute_bytes = (uint64_t) cpu.compute;
        // The dense set left on the host is taken from OUR profile, not from the fitter's `model`
        // term: that term is what the loader would allocate in host backend buffers under the
        // fitter's own load mode, and on a CPU-only machine it equals neither the file nor the
        // dense set (measured: 5724 MiB for a 21242 MiB file whose dense set is 2642). What the
        // fitter knows and we do not is the context and compute reservation, and that is what is
        // taken from it. Dense bytes are pro-rated by the layers still on the host; the embedding
        // and head are counted as host in full, which over-reserves a little where they offload.
        const uint64_t dense_on_host =
            n_layer ? (uint64_t) ((double) model.dense_bytes * (double) (n_layer - on_devices) / (double) n_layer)
                    : model.dense_bytes;
        pl.host_resident_bytes = dense_on_host + (uint64_t) cpu.context + (uint64_t) cpu.compute;
        for (size_t i = 0; i + 1 < dm.size(); ++i)
            pl.device_bytes += (uint64_t) dm[i].model + (uint64_t) dm[i].context + (uint64_t) dm[i].compute;
    }
    return pl;
}

} // namespace bmoe
