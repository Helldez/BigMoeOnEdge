#include "prefill_path.h"

#include "../moe/router_hook.h"

#include <cstdio>
#include <cstring>
#include <exception>

namespace bmoe::detail {

namespace {

// Any valid token serves a graph whose output nobody reads.
llama_token filler_token(const llama_vocab * vocab) {
    const llama_token t = llama_vocab_bos(vocab);
    return t < 0 ? 0 : t;
}

} // namespace

ggml_backend_dev_t PrefillPath::find_device(const std::string & name, std::string & err) {
    if (ggml_backend_dev_t dev = ggml_backend_dev_by_name(name.c_str())) {
        // A registered device can still fail to open: Hexagon registers on any phone with the fastrpc
        // driver, and only opening a session finds a DSP its kernels were not built for. Open it here,
        // before the load, where a failure can still leave the run on the CPU; the backend keeps the
        // session it opened, so the context's own init reuses it.
        std::string what = "no backend";
        ggml_backend_t probe = nullptr;
        try {
            probe = ggml_backend_dev_init(dev, nullptr);
        } catch (const std::exception & e) {
            what = e.what();
        }
        if (probe) {
            ggml_backend_free(probe);
            return dev;
        }
        err = "prefill device '" + name + "' did not open (" + what + ")";
        return nullptr;
    }
    std::string names;
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        if (!names.empty()) names += ", ";
        names += ggml_backend_dev_name(ggml_backend_dev_get(i));
    }
    err = "prefill device '" + name + "' is not available here (devices found: " + names + ")";
    return nullptr;
}

// With no devices given, llama.cpp lists every GPU it finds, and the context opens a backend on each.
// This engine never assigns such a device a layer, so all it can do in the run is pick up ops on the
// host-resident weights, which takes a device that reaches host memory (a host buffer type or buffers
// over host pointers). One with neither, like the Hexagon NPU, is only a session to open, and one that
// may fail to open on a DSP its kernels do not cover: it stays out unless named as the prefill device.
const ggml_backend_dev_t * PrefillPath::devices_without_prefill(std::vector<ggml_backend_dev_t> & keep) {
    std::vector<ggml_backend_dev_t> gpus, igpus;
    bool dropped = false;
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        const enum ggml_backend_dev_type type = ggml_backend_dev_type(dev);
        if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU) continue;
        ggml_backend_dev_props props;
        ggml_backend_dev_get_props(dev, &props);
        if (!props.caps.host_buffer && !props.caps.buffer_from_host_ptr) {
            dropped = true;
            continue;
        }
        // llama.cpp's rule for integrated devices: those of the first backend seen only.
        if (type == GGML_BACKEND_DEVICE_TYPE_GPU) {
            gpus.push_back(dev);
        } else if (igpus.empty() || ggml_backend_dev_backend_reg(dev) == ggml_backend_dev_backend_reg(igpus.back())) {
            igpus.push_back(dev);
        }
    }
    if (!dropped) return nullptr;
    // And integrated devices only when no discrete one is left.
    keep = gpus.empty() ? igpus : gpus;
    keep.push_back(nullptr);
    return keep.data();
}

bool PrefillPath::open(llama_context * ctx,
                       const llama_vocab * vocab,
                       RouterHook & hook,
                       const PrefillDeviceConfig & cfg,
                       const MoeStreamConfig & moe,
                       int n_layer_streamed,
                       std::string & err) {
    hook_ = &hook;
    auto fail = [&](const std::string & what) {
        err = "prefill device " + cfg.device + what;
        return false;
    };

    if (!moe.enabled) {
        hook.begin_capture();
        llama_token tok = filler_token(vocab);
        if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0) return fail(": capture decode failed");
        hook.end_capture();
        llama_memory_clear(llama_get_memory(ctx), true);
    }

    // Layer weights only ("blk.N." is llama.cpp's naming for every architecture). The token table is
    // a row gather and the output head computes the last position alone, so both stay with the CPU:
    // moving them would buy nothing and cost their size in device memory. The usage test drops the
    // graph inputs and KV views the capture records alongside.
    std::vector<ggml_tensor *> layer_weights;
    for (ggml_tensor * t : hook.captured_weight_objects()) {
        if (std::strncmp(t->name, "blk.", 4) != 0) continue;
        if (!t->buffer || ggml_backend_buffer_get_usage(t->buffer) != GGML_BACKEND_BUFFER_USAGE_WEIGHTS) continue;
        layer_weights.push_back(t);
    }
    // Streaming means the model does not fit, and then the layer weights cannot be resident on the
    // device either: on unified memory a resident copy is RAM taken from the expert cache twice over,
    // and a device session has a ceiling of its own. They go through the arena, two layers at a time,
    // paced at each layer's last node — which the capture must have found for every layer, or the
    // pacing has nowhere to wait and the copy stays resident.
    std::vector<std::vector<ggml_tensor *>> dense_per_layer;
    if (moe.enabled && hook.learned_layer_ends()) {
        dense_per_layer.resize((size_t) n_layer_streamed);
        for (ggml_tensor * t : layer_weights) {
            int il = -1;
            if (std::sscanf(t->name, "blk.%d.", &il) == 1 && il >= 0 && il < (int) dense_per_layer.size())
                dense_per_layer[(size_t) il].push_back(t);
        }
        layer_weights.clear();
    } else if (moe.enabled) {
        std::fprintf(stderr, "bmoe: prefill-device: layer ends not found in the graph; layer weights stay "
                             "resident on the device\n");
    }

    dev_ = std::make_unique<PrefillDevice>();
    std::string perr;
    if (!dev_->init(devs_[0], layer_weights, hook.non_matrix_weights(), perr)) return fail(": " + perr);
    std::string state_where;
    if (!dev_->init_state(devs_[0], hook.captured_state_objects(), state_where, perr)) return fail(": " + perr);
    std::fprintf(stderr, "bmoe: prefill-device model state: %.1f MiB in %s\n",
                 (double) dev_->state_bytes() / (1024.0 * 1024.0), state_where.c_str());
    if (moe.enabled && !open_arena(cfg, moe, dense_per_layer, err)) return false;
    hook.count_device_nodes(true);

    if (!reserve_on_device(ctx, vocab, cfg.min_tokens)) return fail(": the device-placed reservation decode failed");
    std::fprintf(stderr, "bmoe: prefill-device %s: %zu layer tensors, %.1f MiB copied; graphs >= %d tokens run there\n",
                 cfg.device.c_str(), dev_->n_tensors(), (double) dev_->bytes() / (1024.0 * 1024.0), cfg.min_tokens);
    return true;
}

bool PrefillPath::open_arena(const PrefillDeviceConfig & cfg,
                             const MoeStreamConfig & moe,
                             const std::vector<std::vector<ggml_tensor *>> & dense_per_layer,
                             std::string & err) {
    arena_ = std::make_unique<DeviceExpertArena>();
    std::string perr;
    if (!arena_->init(devs_[0], arena_shards_, arena_layers_, cfg.load_threads, moe.o_direct, cfg.slots, perr)) {
        err = "prefill device " + cfg.device + " expert arena: " + perr;
        return false;
    }
    arena_layers_.clear();
    arena_->set_test_delay_us(cfg.test_load_delay_us);
    arena_->set_routed(cfg.routed, cfg.routed_full_frac);
    arena_->set_test_skip_demand(cfg.test_routed_skip_demand);
    if (!arena_->init_dense(devs_[0], dense_per_layer, hook_->non_matrix_weights(), perr)) {
        err = "prefill device " + cfg.device + " dense arena: " + perr;
        return false;
    }
    if (arena_->has_dense())
        std::fprintf(stderr,
                     "bmoe: prefill-device dense arena: layer weights through 2 slots, %.1f MiB; %d converted "
                     "to a type the device takes (%.1f MiB)\n",
                     (double) arena_->dense_slot_bytes() / (1024.0 * 1024.0), arena_->dense_converted(),
                     (double) arena_->dense_converted_bytes() / (1024.0 * 1024.0));
    std::fprintf(stderr, "bmoe: prefill-device expert arena: %d layers through %d slot%s of %.1f MiB, %d loaders\n",
                 arena_->n_layers(), arena_->n_slots(), arena_->n_slots() == 1 ? "" : "s",
                 (double) arena_->slot_bytes() / (double) arena_->n_slots() / (1024.0 * 1024.0), cfg.load_threads);
    return true;
}

// The context reserved its compute buffers at creation, for the widest graph with every weight on the
// CPU — a prefill this session will never run there. At a wide ubatch that reservation is gigabytes
// of RAM the expert cache does not get (measured 2.2 GB at 2048 on a 35B-A3B, while the device's own
// graph needed 136 MB at 1024). So reserve again, with the weights where a wide graph will actually
// find them: toggling a context flag is the public way to have llama.cpp redo its reservation at the
// next decode, and one device-placed decode is that decode. The CPU keeps only what its own graphs
// (decode, a short tail) need, grown on demand from there.
bool PrefillPath::reserve_on_device(llama_context * ctx, const llama_vocab * vocab, int n_tokens) {
    llama_set_causal_attn(ctx, false);
    llama_set_causal_attn(ctx, true);
    std::vector<llama_token> toks((size_t) n_tokens, filler_token(vocab));
    place(true);
    const int rc = decode(ctx, llama_batch_get_one(toks.data(), n_tokens));
    place(false);
    llama_memory_clear(llama_get_memory(ctx), true);
    clear_state();
    return rc == 0;
}

void PrefillPath::place(bool on_device) {
    if (!dev_) return;
    dev_->place(on_device);
    if (arena_) {
        arena_->place(on_device);
        hook_->set_device_arena(on_device ? arena_.get() : nullptr);
    }
}

int PrefillPath::decode(llama_context * ctx, const llama_batch & b) {
    const bool dev = arena_ && dev_ && dev_->on_device();
    if (dev) arena_->begin_graph();
    int rc = llama_decode(ctx, b);
    if (dev) {
        arena_->end_graph();
        if (rc == 0 && arena_->failed()) rc = -1;
    }
    return rc;
}

void PrefillPath::clear_state() {
    if (dev_) dev_->clear_state();
}

} // namespace bmoe::detail
