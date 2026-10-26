// The first stage, as an adapter: ask llama.cpp's own capacity fitter what it would do with this
// model on this machine, and read off what the second stage needs from the answer.
//
// This is the one place the planner touches `common/` (llama.cpp's helper library, which upstream
// does not treat as stable API); the engine's chat code under `core/src/engine` uses its chat
// templates, and that is the only other use. The fitter is kept behind a single function returning
// a pure-policy struct so that, when its signature moves under a submodule bump, the blast radius
// is this file and the planner keeps working with `fitted = false` — declining the device axis
// rather than failing to build.

#include "bmoe/config.h"
#include "bmoe/probe.h"

#include "../engine/kv_cache_type.h"
#include "../moe/tensor_names.h"

#include "fit.h"
#include "ggml-backend.h"
#include "llama.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace bmoe {

namespace {
// Below this a graph is not a prefill graph any more, and halving it measures the constant part of
// the buffer rather than the slope.
constexpr uint32_t min_projection_width = 64;

// The same for the context's length: under this, halving it can fall inside a sliding window, whose
// cells stop shrinking with the context, and the fixed part read off the two points would be too big.
constexpr uint32_t min_projection_ctx = 8192;

// What the fitter leaves free on each device. Upstream's own default (`--fit-target`) rather than a
// policy of ours: the second stage applies its own margin to the host budget separately, from what
// a reclaim costs there, and a device margin of our choosing would be a number nothing measured.
constexpr size_t fitter_device_margin_bytes = (size_t) 1 << 30;

// The shortest context the fitter may shrink to before it moves weights instead. Upstream's default
// too (`--fit-ctx`). It binds only when the context is left to the fitter, which this adapter never
// does - the context is pinned below - so it is passed for the call's contract, not as a choice.
constexpr uint32_t fitter_n_ctx_min = 4096;
} // namespace

Placement probe_placement(const char * model_path,
                          const ModelProfile & model,
                          const HardwareProfile & hw,
                          uint32_t n_ctx,
                          uint32_t n_ubatch,
                          KvCacheType type_k,
                          KvCacheType type_v,
                          const std::vector<std::string> & devices,
                          const SpecConfig * spec) {
    Placement pl;
    if (!model_path || !*model_path || !model.ok) return pl;

    // The fitter works by asking every device, and a device that did not survive its own probe is
    // not one to ask again from this process: what ended the probe would end the plan. Nothing is
    // placed, which is also the answer the run needs - that device is being kept out of it.
    for (const ComputeDevice & d : hw.devices)
        if (!d.is_cpu && d.usable == Tri::No) {
            pl.outcome = "the capacity fitter was not run: " + d.name + " failed its probe";
            return pl;
        }

    // The fitter only touches parameters still at their default, and it refuses to run at all if
    // tensor_buft_overrides is already set — so the experts-on-host pin is not something we pass
    // IN; it is what we read OUT of the overrides it writes, and what the session enforces after.
    llama_model_params mparams = llama_model_default_params();
    llama_context_params cparams = llama_context_default_params();
    // The context is always pinned to the caller's. Left at 0 the fitter picks the model's full
    // training context, because it assumes system memory is unlimited - and with no device to fit
    // into, the KV and compute for that context land on the host, the one place that is not.
    cparams.n_ctx = n_ctx ? n_ctx : llama_context_default_params().n_ctx;
    // The widths the session opens with (session_config_from, Session::open): one batch per
    // context, computed in graphs `n_ubatch` wide, or one graph when that is 0. Left at the fitter's
    // defaults, the compute buffers it projects are those of a 512-wide graph whatever the run is
    // about to reserve.
    cparams.n_batch = cparams.n_ctx;
    cparams.n_ubatch = n_ubatch > 0 && n_ubatch < cparams.n_ctx ? n_ubatch : cparams.n_ctx;
    // And the logit rows of a prompt, not of a scoring pass. A context reserves a vocabulary-wide
    // row for every position it may be asked to score, which on a wide graph is most of the
    // reservation (16 GB at 16384 tokens on a 150k vocabulary) and is memory a prompt never touches:
    // it reads one row. What is held is what is touched, so the projection is made with the row cap
    // the session itself sets when a device computes its prefill.
    cparams.n_outputs_max = (uint32_t) PrefillDeviceConfig::max_outputs;
    // The cache's own element types, so the KV it projects is the one the run allocates.
    cparams.type_k = detail::to_ggml_type(type_k);
    cparams.type_v = detail::to_ggml_type(type_v);

    std::vector<float> tensor_split(llama_max_devices(), 0.0f);
    std::vector<llama_model_tensor_buft_override> overrides(llama_max_tensor_buft_overrides());
    for (auto & o : overrides) {
        o.pattern = nullptr;
        o.buft = nullptr;
    }
    std::vector<size_t> margins(llama_max_devices() + 1, fitter_device_margin_bytes);
    std::vector<ggml_backend_dev_t> listed;
    for (const std::string & n : devices)
        if (ggml_backend_dev_t dev = ggml_backend_dev_by_name(n.c_str())) listed.push_back(dev);
    if (!listed.empty()) {
        listed.push_back(nullptr);
        mparams.devices = listed.data();
    }

    // Any speculation keeps rollback snapshots of the recurrent state, one per drafted position, in
    // the target's own context (Session::open).
    if (spec && spec->enabled()) cparams.n_rs_seq = (uint32_t) spec->draft_max;
    // The MTP draft context, opened as the session opens it: its head is the target's own block
    // (weights already counted, the target loaded with it) or a file of its own on the host, and
    // either way its state and compute buffers land on the devices beside the target's.
    const bool mtp = spec && spec->is_mtp();
    const bool own_head = mtp && !spec->mtp_head.empty();
    mparams.load_mtp = mtp && !own_head;
    llama_model_params dmparams = llama_model_default_params();
    llama_context_params dcparams = cparams;
    common_fit_extra_model draft{};
    if (mtp) {
        dmparams.load_mtp = true;
        if (own_head) dmparams.n_gpu_layers = 0;
        dcparams.ctx_type = LLAMA_CONTEXT_TYPE_MTP;
        dcparams.moe_cache_size = 0;
        dcparams.n_rs_seq = 0;
        dcparams.n_outputs_max = 0;
        dcparams.n_ubatch = std::min<uint32_t>(cparams.n_ubatch,
                                               (uint32_t) std::max(spec->draft_max + 1, SpecConfig::mtp_draft_ubatch));
        draft.path_model = own_head ? spec->mtp_head.c_str() : model_path;
        draft.mparams = &dmparams;
        draft.cparams = &dcparams;
        draft.shares_model = !own_head;
    }

    const common_params_fit_status st =
        common_fit_params(model_path, &mparams, &cparams, tensor_split.data(), overrides.data(), margins.data(),
                          fitter_n_ctx_min, mtp ? &draft : nullptr, GGML_LOG_LEVEL_WARN);
    if (st == COMMON_PARAMS_FIT_STATUS_ERROR) {
        pl.outcome = "the capacity fitter hit an error; planning as if there were no devices";
        return pl;
    }
    pl.fitted = true;
    pl.outcome = st == COMMON_PARAMS_FIT_STATUS_SUCCESS
                     ? "the capacity fitter found a placement that is projected to fit"
                     : "the capacity fitter could not make the model fit the devices";
    pl.n_ctx = cparams.n_ctx;

    // With no device at all, "layers on devices" means nothing: the fitter leaves n_gpu_layers at
    // its default, which means "all", and taken literally that would read as every expert placed
    // off-host on a machine that has nowhere to put them. So the number is trusted only when there
    // is something to place on.
    //
    // "Something" means a device with memory OF ITS OWN, and the reason is a phone that crashed.
    //
    // The first version of this counted `TYPE_GPU` only, so an integrated accelerator - which
    // reports `TYPE_IGPU` - never counted and the fitter's placement was discarded on every machine
    // with one. That looked like a bug and was changed to count any non-CPU device. It is a bug,
    // and it is not the one that matters: on unified memory the fitter's accounting DOUBLE-COUNTS.
    // It was told an Adreno had "15195 MiB free" on a phone with 11 GB in total, because the GPU's
    // memory IS the host's, and it then placed 14125 MiB there while leaving 1810 MiB of host set
    // beside it. Honouring that number made the session try to do it, and took the device down.
    //
    // So the fitter is trusted only where its capacity arithmetic is sound: a device with separate
    // memory. Where the memory is shared, its placement is a projection about a pool it has counted
    // twice, and this planner does not act on it until `device_bytes` is charged to the same budget
    // as the host set - which is a change to the second stage, not a flag here.
    // Counted from the PROFILE's measured fact, not from the device type. The type was the whole
    // bug: an integrated accelerator reports TYPE_IGPU and never counted, and Metal reports TYPE_GPU
    // on hardware whose memory is unified, so counting types either discards a real placement or
    // credits a shared pool as separate capacity. `has_own_memory()` is true only where the probe
    // established it by allocating on the device and watching what the host lost - and an unsettled
    // device reads as shared, which is the safe direction.
    //
    // And it is asked of the devices the fitter PUT BYTES ON, not of the machine. A device with
    // memory of its own somewhere else in the room says nothing about the one that was given the
    // layers: on a phone an NPU measured as not drawing on the host (its buffers come from a kernel
    // allocator this process cannot see) let the fitter put 41 layers on the GPU beside it, whose
    // memory is the host's, and the plan applied 2106 MiB "on devices" that its ledger never held.
    // "Its own" also needs a size, for the reason the prefill rule gives: a pool with no stated
    // size is not one the fitter can have budgeted against.
    std::vector<ggml_backend_dev_t> devs;
    uint32_t hp_ngl = 0, hp_nct = 0, hp_nex = 0;
    const common_device_memory_data_vec dm = common_get_device_memory_data(model_path, &mparams, &cparams, devs, hp_ngl,
                                                                           hp_nct, hp_nex, GGML_LOG_LEVEL_WARN);
    size_t n_dev_local = 0;
    for (const ComputeDevice & d : hw.devices)
        if (d.has_own_memory() && d.memory_total > 0) ++n_dev_local;
    const ComputeDevice * placed_on_shared = nullptr;
    bool placed_unknown = false;
    for (size_t i = 0; i + 1 < dm.size() && i < devs.size(); ++i) {
        if (dm[i].model + dm[i].context + dm[i].compute == 0) continue;
        const char * name = ggml_backend_dev_name(devs[i]);
        const ComputeDevice * facts = nullptr;
        for (const ComputeDevice & d : hw.devices)
            if (!d.is_cpu && name && d.name == name) facts = &d;
        if (!facts)
            placed_unknown = true; // not in the profile: nothing says its memory is its own
        else if (!(facts->has_own_memory() && facts->memory_total > 0) && !placed_on_shared)
            placed_on_shared = facts;
    }
    if (placed_on_shared || placed_unknown) n_dev_local = 0;
    pl.n_gpu_layers = n_dev_local ? mparams.n_gpu_layers : 0;
    // The fitter writes one layer count per device into tensor_split only when there are several;
    // with one, every placed layer is that device's.
    if (pl.n_gpu_layers > 0) {
        for (size_t i = 0; i < devs.size(); ++i) {
            const float n = devs.size() > 1 ? tensor_split[i] : (float) pl.n_gpu_layers;
            if (n > 0.0f) pl.device_split.emplace_back(ggml_backend_dev_name(devs[i]), (uint32_t) n);
        }
    }

    // Where no device has memory of its own, the whole first stage has nothing to say. A capacity
    // fitter answers "what fits where" by moving bytes between pools, and there is one pool: moving
    // a weight onto such a device frees nothing, and the numbers it produces describe a machine that
    // does not exist - it was told an integrated accelerator had 15 GB free on an 11 GB device, and
    // placed 14125 MiB there beside a host set it counted separately.
    //
    // Neither honouring that (which took a phone down) nor charging it to the real pool (which then
    // reads as a 34 GB model and declines a run that works) is right, because the number is not
    // wrong by an amount - it is about a distinction this machine does not have. So the stage is
    // reported as not fitted and the plan proceeds from the model alone, which is what it does on a
    // machine with no accelerator at all and is exactly right here. Using such a device is a
    // BANDWIDTH decision, and that one is the second stage's to make.
    const bool shared_memory_only = n_dev_local == 0 && ggml_backend_dev_count() > 1;

    // Which layers keep their routed experts on the host. Two sources, and both are read rather
    // than reconstructed: a layer beyond what the fitter placed on devices is entirely host; a
    // placed layer an override moves any of the FFN of is host for all of its experts (below).
    // Anything else went to a device and is not the streamer's to serve.
    const uint32_t n_layer = model.n_layer;
    const uint32_t on_devices = pl.n_gpu_layers < 0 ? n_layer : std::min<uint32_t>((uint32_t) pl.n_gpu_layers, n_layer);
    const uint32_t first_device_layer = n_layer - on_devices; // llama.cpp fills devices from the top

    std::vector<bool> host(n_layer, false);
    for (uint32_t il = 0; il < first_device_layer; ++il)
        host[il] = true;

    // A layer's experts never straddle the boundary. The fitter's partial layer can overflow a SHARE
    // of its FFN ("blk\.11\.ffn_down.*": down here, gate and up on the device), and the session routes
    // every pattern to the host whatever device it named. That leaves one projection of a layer's
    // experts in host memory and the others on a device: the streamer can serve neither half, and on
    // a phone it aborted the run at capture. So any pattern that moves part of a layer's FFN brings
    // all of that layer's experts with it. They are then streamed, which the host pays for through
    // the cache rather than by holding them, and the device holds less than the fitter projected.
    for (const auto & o : overrides) {
        if (!o.pattern) break;
        pl.override_patterns.push_back(o.pattern);
        const int il = tensor_names::ffn_override_layer(o.pattern);
        if (il < 0 || (uint32_t) il >= n_layer) continue;
        if (tensor_names::expert_override_layer(o.pattern) != il)
            pl.override_patterns.push_back(tensor_names::expert_override_pattern(il));
        host[(size_t) il] = true;
    }
    // A block that carries no experts has none to leave on the host: on a hybrid or a leading-dense
    // architecture the list would otherwise count every block, and a count of 52 "MoE layers" out of
    // 23 prorates the expert set past itself. Where the profile does not say, every block counts.
    for (uint32_t il = 0; il < n_layer; ++il) {
        if (!model.moe_layers.empty() &&
            std::find(model.moe_layers.begin(), model.moe_layers.end(), il) == model.moe_layers.end())
            continue;
        pl.all_expert_patterns.push_back(tensor_names::expert_override_pattern((int) il));
        if (host[il]) pl.host_expert_layers.push_back(il);
    }

    // The host memory the placed model is projected to take. The breakdown's last entry is the
    // CPU; its `model` term still counts the experts that stayed on the host, which the streamer
    // takes off the books — so those are subtracted, pro rata by layer.
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
        for (size_t i = 0; i + 1 < dm.size(); ++i) {
            pl.device_bytes += (uint64_t) dm[i].model + (uint64_t) dm[i].context + (uint64_t) dm[i].compute;
            pl.device_compute_bytes += (uint64_t) dm[i].compute;
            pl.device_context_bytes += (uint64_t) dm[i].context;
            if (dm[i].compute && i < devs.size() && ggml_backend_dev_name(devs[i])) {
                pl.projected_devices.push_back(ggml_backend_dev_name(devs[i]));
                pl.projected_compute_bytes.push_back((uint64_t) dm[i].compute);
            }
        }
        pl.compute_width = cparams.n_ubatch;

        // The same placement, half as wide: two points of a line. A compute buffer is activations,
        // a row per token, so the difference between the two is what a token of width costs here -
        // on this model, this context and these backends, with nothing assumed about any of them.
        if (cparams.n_ubatch >= 2 * min_projection_width) {
            llama_context_params half = cparams;
            half.n_ubatch = cparams.n_ubatch / 2;
            std::vector<ggml_backend_dev_t> devs_half;
            uint32_t a = 0, b = 0, c = 0;
            const common_device_memory_data_vec hm =
                common_get_device_memory_data(model_path, &mparams, &half, devs_half, a, b, c, GGML_LOG_LEVEL_WARN);
            if (hm.size() == dm.size()) {
                const uint64_t span = cparams.n_ubatch - half.n_ubatch;
                uint64_t dev_half = 0;
                for (size_t i = 0; i + 1 < hm.size(); ++i)
                    dev_half += (uint64_t) hm[i].compute;
                const uint64_t host_half = (uint64_t) hm.back().compute;
                if (pl.raw_host_compute_bytes > host_half)
                    pl.host_compute_per_token = (pl.raw_host_compute_bytes - host_half) / span;
                if (pl.device_compute_bytes > dev_half)
                    pl.device_compute_per_token = (pl.device_compute_bytes - dev_half) / span;
            }
        }

        // The same placement, at half the context: the context is a fixed part plus cells that grow
        // with the length, so twice the half minus the whole is the part that does not grow. That is
        // the state a context checkpoint copies (a recurrent state, a sliding window), measured on
        // this model and these types with nothing read from the architecture.
        if (cparams.n_ctx >= 2 * min_projection_ctx) {
            llama_context_params half = cparams;
            half.n_ctx = cparams.n_ctx / 2;
            half.n_batch = half.n_ctx;
            half.n_ubatch = std::min(cparams.n_ubatch, half.n_ctx);
            std::vector<ggml_backend_dev_t> devs_half;
            uint32_t a = 0, b = 0, c = 0;
            const common_device_memory_data_vec hm =
                common_get_device_memory_data(model_path, &mparams, &half, devs_half, a, b, c, GGML_LOG_LEVEL_WARN);
            if (hm.size() == dm.size()) {
                uint64_t whole = 0, halved = 0;
                for (size_t i = 0; i < dm.size(); ++i) {
                    whole += (uint64_t) dm[i].context;
                    halved += (uint64_t) hm[i].context;
                }
                pl.context_fixed_bytes = 2 * halved > whole ? 2 * halved - whole : 0;
            }
        }
    }

    // Set aside last, with its numbers kept. Everything above ran, so the plan can quote what the
    // fitter said while declining to act on it - a decision reported without its figures is one a
    // reader has to take on trust, and this one in particular deserves to be checkable.
    if (shared_memory_only) {
        pl.shared_memory_placement = true;
        pl.fitted = false;
        pl.outcome = placed_on_shared ? "the capacity fitter ran, and placed layers on " + placed_on_shared->name +
                                            ", whose memory is this host's own"
                                      : "the capacity fitter ran, and had only devices whose memory is this host's own";
    }
    return pl;
}

} // namespace bmoe
