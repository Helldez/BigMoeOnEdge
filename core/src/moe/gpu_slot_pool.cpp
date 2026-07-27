#include "gpu_slot_pool.h"

#include "../io/file_reader.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef BMOE_HAVE_OPENCL_SLOT_HOOK
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-opencl.h"
#include "ggml.h"
#endif

namespace bmoe {

#ifndef BMOE_HAVE_OPENCL_SLOT_HOOK

GpuSlotPool::~GpuSlotPool() = default;

bool GpuSlotPool::init(std::vector<LayerExperts>, int, int, FileReader *) {
    error_ = "this build has no OpenCL expert slot hook";
    return false;
}
void GpuSlotPool::arm_for_capture(int) {}
void GpuSlotPool::install() {}
void GpuSlotPool::uninstall() {}
int GpuSlotPool::ensure_resident(int, int) {
    return -1;
}
GpuSlotPool::IdsBuffer * GpuSlotPool::ids_buffer(const ggml_tensor *, int64_t, int64_t) {
    return nullptr;
}
bool GpuSlotPool::c_hook(const ggml_tensor *, const ggml_tensor *, ggml_opencl_moe_slots *, void *) {
    return false;
}
bool GpuSlotPool::on_hook(const ggml_tensor *, const ggml_tensor *, ggml_opencl_moe_slots *) {
    return false;
}

#else

static GpuSlotPool * g_installed = nullptr;

GpuSlotPool::~GpuSlotPool() {
    uninstall();
    for (auto & b : ids_bufs_) {
        if (b.buf) ggml_backend_buffer_free(b.buf);
        if (b.ctx) ggml_free(b.ctx);
    }
}

bool GpuSlotPool::init(std::vector<LayerExperts> layers, int n_expert, int n_slots, FileReader * reader) {
    if (!reader || n_slots <= 0 || n_expert <= 0) {
        error_ = "gpu slot pool: invalid arguments";
        return false;
    }
    if (n_slots > n_expert) {
        error_ = "gpu slot pool: more slots than experts";
        return false;
    }

    layers_ = std::move(layers);
    reader_ = reader;
    n_expert_ = n_expert;
    n_slots_ = n_slots;
    slots_.assign(layers_.size(), LayerSlots{});

    size_t max_nb2 = 0;
    for (size_t il = 0; il < layers_.size(); ++il) {
        const LayerExperts & L = layers_[il];
        if (!L.bound) continue;

        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            ggml_tensor * t = L.proj[p].tensor;
            if (!t) continue;

            // The pool only addresses slots. A tensor still carrying every expert means the model
            // was loaded without moe_expert_slots, and writing slot i would land on expert i --
            // arithmetically wrong in a way that produces plausible tokens rather than a crash.
            if (t->ne[2] != n_slots) {
                error_ = std::string("gpu slot pool: ") + ggml_get_name(t) + " has " + std::to_string(t->ne[2]) +
                         " experts, expected " + std::to_string(n_slots) + " (model not loaded with moe_expert_slots)";
                return false;
            }
            if (!t->buffer || ggml_backend_buffer_is_host(t->buffer)) {
                error_ = std::string("gpu slot pool: ") + ggml_get_name(t) + " is not in device memory";
                return false;
            }
            // Refused here rather than at the first upload: a slice write that fails mid-decode
            // leaves the routing pointing at whatever the slot held, which reads as fluent nonsense
            // instead of an error.
            if (t->type != GGML_TYPE_Q4_0 && t->type != GGML_TYPE_Q4_1) {
                error_ = std::string("gpu slot pool: ") + ggml_get_name(t) + " is " + ggml_type_name(t->type) +
                         "; only Q4_0 and Q4_1 experts can be written a slice at a time";
                return false;
            }
            tensor_layer_[t] = (int) il;
            max_nb2 = std::max(max_nb2, (size_t) L.proj[p].nb2);
        }

        LayerSlots & S = slots_[il];
        S.slot_expert.assign(n_slots, -1);
        S.slot_lru.resize(n_slots);
        for (int s = 0; s < n_slots; ++s) {
            S.slot_lru[s] = s;
        }
    }

    if (tensor_layer_.empty()) {
        error_ = "gpu slot pool: no expert tensors in device memory";
        return false;
    }
    staging_.resize(max_nb2);

    // The load already wrote the file's first n_slots experts into each pooled tensor, which both
    // seeds the pool and builds the quantized layout a per-slice write addresses. Record that
    // starting occupancy rather than re-uploading it.
    for (size_t il = 0; il < layers_.size(); ++il) {
        if (!layers_[il].bound) continue;
        LayerSlots & S = slots_[il];
        for (int s = 0; s < n_slots; ++s) {
            S.slot_expert[s] = s;
            S.expert_slot[s] = s;
        }
    }

    capturing_ = false;
    return true;
}

void GpuSlotPool::arm_for_capture(int n_slots) {
    n_slots_ = n_slots;
    capturing_ = true;
    install();
}

void GpuSlotPool::install() {
    if (installed_) return;
    g_installed = this;
    ggml_backend_opencl_set_moe_slot_hook(&GpuSlotPool::c_hook, this);
    installed_ = true;
}

void GpuSlotPool::uninstall() {
    if (!installed_) return;
    ggml_backend_opencl_set_moe_slot_hook(nullptr, nullptr);
    g_installed = nullptr;
    installed_ = false;
}

int GpuSlotPool::ensure_resident(int il, int expert) {
    LayerSlots & S = slots_[il];

    auto touch = [&S](int slot) {
        auto it = std::find(S.slot_lru.begin(), S.slot_lru.end(), slot);
        if (it != S.slot_lru.end()) S.slot_lru.erase(it);
        S.slot_lru.push_back(slot);
    };

    auto hit = S.expert_slot.find(expert);
    if (hit != S.expert_slot.end()) {
        stats_.hits++;
        touch(hit->second);
        return hit->second;
    }

    stats_.misses++;
    const int slot = S.slot_lru.front();
    const int victim = S.slot_expert[slot];
    if (victim >= 0) S.expert_slot.erase(victim);

    const LayerExperts & L = layers_[il];
    for (int p = 0; p < MoeRecipe::max_exps; ++p) {
        ggml_tensor * t = L.proj[p].tensor;
        if (!t) continue;

        const uint64_t nb2 = L.proj[p].nb2;
        // The reader reports what it pulled from the drive, which under O_DIRECT is the requested
        // range rounded out to alignment - so it is >= nb2, and only a negative value is a failure.
        const auto tr0 = std::chrono::steady_clock::now();
        const long long got = reader_->read(0, staging_.data(), L.proj[p].file_off + (uint64_t) expert * nb2, nb2);
        const auto tr1 = std::chrono::steady_clock::now();
        stats_.read_ns += (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(tr1 - tr0).count();
        if (got < 0) {
            S.slot_expert[slot] = -1; // the slot now holds a partially written expert
            return -1;
        }
        const bool wrote = ggml_backend_opencl_set_expert_slice(t, slot, staging_.data(), (size_t) nb2);
        stats_.upload_ns +=
            (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tr1)
                .count();
        if (!wrote) {
            fprintf(stderr, "gpu slot pool: slice write refused for %s (type %s, ne=%lld,%lld,%lld) slot %d\n",
                    ggml_get_name(t), ggml_type_name(t->type), (long long) t->ne[0], (long long) t->ne[1],
                    (long long) t->ne[2], slot);
            S.slot_expert[slot] = -1;
            return -1;
        }
        stats_.bytes_up += nb2;
    }

    S.slot_expert[slot] = expert;
    S.expert_slot[expert] = slot;
    touch(slot);
    return slot;
}

GpuSlotPool::IdsBuffer * GpuSlotPool::ids_buffer(const ggml_tensor * like, int64_t ne0, int64_t ne1) {
    for (auto & b : ids_bufs_) {
        if (b.ne0 == ne0 && b.ne1 == ne1) return &b;
    }

    // Allocated in the same buffer type as the experts, so the substitute reaches the dispatch the
    // same way the routing would have.
    ggml_backend_buffer_type_t buft = ggml_backend_buffer_get_type(like->buffer);

    ggml_init_params ip = {};
    ip.mem_size = 2 * ggml_tensor_overhead();
    ip.no_alloc = true;
    IdsBuffer b;
    b.ne0 = ne0;
    b.ne1 = ne1;
    b.ctx = ggml_init(ip);
    if (!b.ctx) return nullptr;

    b.base = ggml_new_tensor_2d(b.ctx, GGML_TYPE_I32, n_slots_, ne1);
    ggml_set_name(b.base, "bmoe.slot_ids");
    b.view = ggml_view_2d(b.ctx, b.base, ne0, ne1, (size_t) n_slots_ * sizeof(int32_t), 0);
    ggml_set_name(b.view, "bmoe.slot_ids.view");

    b.buf = ggml_backend_alloc_ctx_tensors_from_buft(b.ctx, buft);
    if (!b.buf) {
        ggml_free(b.ctx);
        return nullptr;
    }
    ids_bufs_.push_back(b);
    return &ids_bufs_.back();
}

bool GpuSlotPool::c_hook(const ggml_tensor * weights,
                         const ggml_tensor * ids,
                         ggml_opencl_moe_slots * out,
                         void * user_data) {
    return static_cast<GpuSlotPool *>(user_data)->on_hook(weights, ids, out);
}

bool GpuSlotPool::on_hook(const ggml_tensor * weights, const ggml_tensor * ids, ggml_opencl_moe_slots * out) {
    if (capturing_) {
        // Nothing is known about the layers yet, so recognise a pooled tensor by its narrowed
        // expert dimension and fold the routing into the slots that exist.
        if (weights->ne[2] != n_slots_) return false;
        IdsBuffer * b = ids_buffer(weights, ids->ne[0], ids->ne[1]);
        if (!b) return false;

        const int64_t nid = ids->ne[0];
        const int64_t rows = ids->ne[1];
        host_ids_.resize((size_t) (nid * rows));
        ggml_backend_tensor_get(const_cast<ggml_tensor *>(ids), host_ids_.data(), 0,
                                host_ids_.size() * sizeof(int32_t));

        mapped_ids_.assign((size_t) (n_slots_ * rows), 0);
        for (int64_t r = 0; r < rows; ++r) {
            for (int64_t i = 0; i < nid; ++i) {
                const int32_t e = host_ids_[(size_t) (r * nid + i)];
                mapped_ids_[(size_t) (r * n_slots_ + i)] = (e > 0 && n_slots_ > 0) ? (e % n_slots_) : 0;
            }
        }
        ggml_backend_tensor_set(b->base, mapped_ids_.data(), 0, mapped_ids_.size() * sizeof(int32_t));
        out->weights = weights;
        out->ids = b->view;
        return true;
    }

    auto it = tensor_layer_.find(weights);
    if (it == tensor_layer_.end()) {
        return false; // not ours: a dense matmul, or a model loaded without slots
    }
    const int il = it->second;

    const auto t0 = std::chrono::steady_clock::now();
    stats_.lookups++;

    IdsBuffer * b = ids_buffer(weights, ids->ne[0], ids->ne[1]);
    if (!b) return false;

    // The three projections of a layer are dispatched against the same routing, so only the first
    // of them pays the readback.
    if (!(cached_layer_ == il && cached_ids_ == ids)) {
        const int64_t nid = ids->ne[0];
        const int64_t rows = ids->ne[1];
        host_ids_.resize((size_t) (nid * rows));
        ggml_backend_tensor_get(const_cast<ggml_tensor *>(ids), host_ids_.data(), 0,
                                host_ids_.size() * sizeof(int32_t));
        stats_.readbacks++;

        mapped_ids_.assign((size_t) (n_slots_ * rows), 0);
        for (int64_t r = 0; r < rows; ++r) {
            for (int64_t i = 0; i < nid; ++i) {
                const int32_t e = host_ids_[(size_t) (r * nid + i)];
                // Out-of-range ids are padding llama.cpp leaves in the routing; they select nothing,
                // so any resident slot is a safe answer and costs no upload.
                if (e < 0 || e >= n_expert_) {
                    continue;
                }
                const int slot = ensure_resident(il, e);
                // Falling back to the untouched routing would index the pool with a model-wide
                // expert id and read past it, so a failed upload has to stop the run.
                if (slot < 0) GGML_ABORT("gpu slot pool: could not make expert %d of layer %d resident", e, il);
                mapped_ids_[(size_t) (r * n_slots_ + i)] = slot;
            }
        }
        ggml_backend_tensor_set(b->base, mapped_ids_.data(), 0, mapped_ids_.size() * sizeof(int32_t));

        cached_layer_ = il;
        cached_ids_ = ids;
    }

    out->weights = weights; // the model's tensor already IS the pool
    out->ids = b->view;

    stats_.stall_ns +=
        (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

#endif // BMOE_HAVE_OPENCL_SLOT_HOOK

} // namespace bmoe
