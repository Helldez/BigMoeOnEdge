#include "device_arena.h"

#include "../io/platform_io.h"

#include "ggml-alloc.h"
#include "ggml.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace bmoe {

namespace {
constexpr size_t kAlign = 4096; // O_DIRECT alignment, as the streamer uses
}

DeviceExpertArena::~DeviceExpertArena() {
    place(false);
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        queue_.clear();
    }
    cv_work_.notify_all();
    for (std::thread & t : threads_)
        if (t.joinable()) t.join();
    for (void * s : staging_)
        if (s) pio::aligned_free(s);
    if (buf_) ggml_backend_buffer_free(buf_);
    if (ctx_) ggml_free(ctx_);
}

bool DeviceExpertArena::init(ggml_backend_dev_t dev,
                             const std::vector<std::string> & shard_paths,
                             const std::vector<LayerExperts> & layers,
                             int threads,
                             bool direct,
                             std::string & err) {
    if (!dev || threads < 1) {
        err = "no device or no loader thread";
        return false;
    }

    // The bound layers, in graph order, and the shape every one of them must share.
    const ggml_tensor * shape[MoeRecipe::max_exps] = {};
    k_of_layer_.assign(layers.size(), -1);
    uint64_t max_nb2 = 0;
    for (size_t il = 0; il < layers.size(); ++il) {
        const LayerExperts & L = layers[il];
        if (!L.bound) continue;
        Layer a;
        a.il = (int) il;
        int np = 0;
        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            ggml_tensor * t = L.proj[p].tensor;
            if (!t) break; // recipes fill their slots from the front
            if (!shape[p]) {
                shape[p] = t;
            } else if (t->type != shape[p]->type || !ggml_are_same_shape(t, shape[p])) {
                err = std::string("expert tensor ") + t->name + " differs in shape or type from " + shape[p]->name +
                      "; the device slots need every MoE layer alike";
                return false;
            }
            a.t[p] = t;
            a.proj[p].file_off = L.proj[p].file_off;
            a.proj[p].nb2 = (uint64_t) t->nb[2];
            a.proj[p].file_idx = L.proj[p].file_idx;
            if (a.proj[p].file_idx < 0 || a.proj[p].file_idx >= (int) shard_paths.size()) {
                err = std::string("expert tensor ") + t->name + " names a shard that does not exist";
                return false;
            }
            max_nb2 = std::max(max_nb2, a.proj[p].nb2);
            ++np;
        }
        if (n_proj_ == 0) n_proj_ = np;
        if (np != n_proj_ || np == 0) {
            err = "MoE layers disagree on how many expert tensors they have";
            return false;
        }
        k_of_layer_[il] = (int) order_.size();
        order_.push_back(a);
    }
    if (order_.empty()) {
        err = "no MoE layer to stream";
        return false;
    }
    n_expert_ = (int) shape[0]->ne[2];

    // Two slots, plus a view per expert into each: the loaders write experts one at a time through
    // the views, so a slot fills in parallel and never needs a layer-sized staging buffer.
    const size_t n_tensors = 2 * (size_t) n_proj_ * (1 + (size_t) n_expert_);
    ggml_init_params ip{};
    ip.mem_size = ggml_tensor_overhead() * (n_tensors + 8);
    ip.no_alloc = true;
    ctx_ = ggml_init(ip);
    if (!ctx_) {
        err = "ggml_init failed";
        return false;
    }
    ggml_backend_buffer_type_t buft = ggml_backend_dev_buffer_type(dev);
    for (int s = 0; s < 2; ++s) {
        for (int p = 0; p < n_proj_; ++p) {
            ggml_tensor * t = ggml_dup_tensor(ctx_, shape[p]);
            ggml_format_name(t, "arena.s%d.p%d", s, p);
            // A backend that pads rows for its own layout (Hexagon rounds both matrix dims up to 32)
            // would put expert e somewhere other than e * nb2, where the views write it.
            if (ggml_backend_buft_get_alloc_size(buft, t) != ggml_nbytes(t)) {
                err = std::string("the device pads expert matrices of ") + shape[p]->name +
                      "; per-expert upload needs them unpadded";
                return false;
            }
            slot_[s][p] = t;
            views_[s][p].resize((size_t) n_expert_);
            for (int e = 0; e < n_expert_; ++e) {
                ggml_tensor * v =
                    ggml_view_3d(ctx_, t, t->ne[0], t->ne[1], 1, t->nb[1], t->nb[2], (size_t) e * t->nb[2]);
                views_[s][p][(size_t) e] = v;
            }
            slot_bytes_ += ggml_nbytes(t);
        }
    }
    buf_ = ggml_backend_alloc_ctx_tensors_from_buft(ctx_, buft);
    if (!buf_) {
        err = std::string("cannot allocate two expert slots on ") + ggml_backend_dev_name(dev);
        return false;
    }
    ggml_backend_buffer_set_usage(buf_, GGML_BACKEND_BUFFER_USAGE_WEIGHTS);

    // One whole-tensor write per slot tensor, before any view write. A backend records how a tensor
    // is laid out when it is written (Hexagon flags it repacked), and the op reads the slot tensor,
    // not the views the loaders write through — so the slot itself must have been written once.
    {
        size_t zmax = 0;
        for (int p = 0; p < n_proj_; ++p)
            zmax = std::max(zmax, ggml_nbytes(slot_[0][p]));
        std::vector<uint8_t> zeros(zmax, 0);
        for (int s = 0; s < 2; ++s)
            for (int p = 0; p < n_proj_; ++p)
                ggml_backend_tensor_set(slot_[s][p], zeros.data(), 0, ggml_nbytes(slot_[s][p]));
    }

    for (const std::string & sp : shard_paths) {
        readers_.push_back(std::unique_ptr<FileReader>(new FileReader()));
        if (!readers_.back()->open(sp, threads, direct, kAlign, (size_t) max_nb2 + 2 * kAlign)) {
            err = "cannot open " + sp;
            return false;
        }
    }
    staging_.assign((size_t) threads, nullptr);
    for (int i = 0; i < threads; ++i) {
        staging_[(size_t) i] = pio::alloc_aligned(kAlign, (size_t) max_nb2);
        if (!staging_[(size_t) i]) {
            err = "cannot allocate loader staging";
            return false;
        }
    }
    remaining_.assign(order_.size(), 0);
    scheduled_.assign(order_.size(), false);
    for (int i = 0; i < threads; ++i)
        threads_.emplace_back(&DeviceExpertArena::worker, this, i);
    return true;
}

void DeviceExpertArena::place(bool on_device) {
    if (on_device == on_device_) return;
    for (size_t k = 0; k < order_.size(); ++k) {
        Layer & L = order_[k];
        for (int p = 0; p < n_proj_; ++p) {
            ggml_tensor * t = L.t[p];
            if (on_device) {
                L.host_buffer[p] = t->buffer;
                L.host_data[p] = t->data;
                L.host_extra[p] = t->extra;
                const ggml_tensor * s = slot_[k % 2][p];
                t->buffer = s->buffer;
                t->data = s->data;
                t->extra = s->extra;
            } else {
                t->buffer = L.host_buffer[p];
                t->data = L.host_data[p];
                t->extra = L.host_extra[p];
            }
        }
    }
    on_device_ = on_device;
}

void DeviceExpertArena::schedule(int k) {
    if (k < 0 || k >= (int) order_.size() || scheduled_[(size_t) k]) return;
    scheduled_[(size_t) k] = true;
    remaining_[(size_t) k] = n_proj_ * n_expert_;
    in_flight_ += n_proj_ * n_expert_;
    // Projection-major, the order the layer's matmuls consume them in.
    for (int p = 0; p < n_proj_; ++p)
        for (int e = 0; e < n_expert_; ++e)
            queue_.push_back(Task{k, p, e});
    cv_work_.notify_all();
}

void DeviceExpertArena::begin_graph() {
    std::lock_guard<std::mutex> lk(mu_);
    std::fill(scheduled_.begin(), scheduled_.end(), false);
    schedule(0);
    schedule(1);
}

void DeviceExpertArena::barrier(int il) {
    if (il < 0 || il >= (int) k_of_layer_.size()) return;
    const int k = k_of_layer_[(size_t) il];
    if (k < 0) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(mu_);
    // The graph has finished everything before this layer's routing, so layer k-1 is done with the
    // slot k+1 shares with it.
    schedule(k);
    schedule(k + 1);
    cv_done_.wait(lk, [&] { return remaining_[(size_t) k] == 0; });
    lk.unlock();
    stall_ns_ +=
        (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}

void DeviceExpertArena::end_graph() {
    std::unique_lock<std::mutex> lk(mu_);
    // A graph that stopped early leaves queued reads nobody will wait for: drop them, and wait only
    // for the ones a loader already holds.
    for (const Task & t : queue_) {
        --remaining_[(size_t) t.k];
        --in_flight_;
    }
    queue_.clear();
    cv_done_.wait(lk, [&] { return in_flight_ == 0; });
}

void DeviceExpertArena::worker(int lane) {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_work_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            task = queue_.front();
            queue_.pop_front();
        }
        // Once per layer (its first upload): with one loader that holds the whole layer back, and it
        // stays cheap where the sleep granularity is coarse (Windows rounds up to ~15 ms).
        if (test_delay_us_ > 0 && task.p == 0 && task.e == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(test_delay_us_));
        const Layer & L = order_[(size_t) task.k];
        const Proj & pr = L.proj[task.p];
        void * stage = staging_[(size_t) lane];
        const long long got =
            readers_[(size_t) pr.file_idx]->read(lane, stage, pr.file_off + (uint64_t) task.e * pr.nb2, pr.nb2);
        if (got < 0) {
            failed_ = true;
        } else {
            ggml_backend_tensor_set(views_[task.k % 2][task.p][(size_t) task.e], stage, 0, (size_t) pr.nb2);
            read_bytes_ += pr.nb2;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            --remaining_[(size_t) task.k];
            --in_flight_;
        }
        cv_done_.notify_all();
    }
}

} // namespace bmoe
