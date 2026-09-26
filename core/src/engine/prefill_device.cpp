#include "prefill_device.h"

#include "ggml-alloc.h"
#include "ggml.h"

namespace bmoe::detail {

PrefillDevice::~PrefillDevice() {
    // The model outlives nothing it cannot read: hand every weight back to its mapping before the
    // device copy it may still point at is freed.
    to_host();
    for (const Entry & e : states_)
        apply(e.t, e.host);
    if (buf_) ggml_backend_buffer_free(buf_);
    if (ctx_) ggml_free(ctx_);
    if (state_buf_) ggml_backend_buffer_free(state_buf_);
    if (state_ctx_) ggml_free(state_ctx_);
}

bool PrefillDevice::init_state(ggml_backend_dev_t dev,
                               const std::vector<ggml_tensor *> & states,
                               std::string & where,
                               std::string & err) {
    if (states.empty()) {
        where = "none";
        return true;
    }
    ggml_backend_buffer_type_t buft = ggml_backend_dev_host_buffer_type(dev);
    if (!buft) buft = ggml_backend_dev_buffer_type(ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU));
    where = ggml_backend_buft_name(buft);

    ggml_init_params ip{};
    ip.mem_size = ggml_tensor_overhead() * (states.size() + 1);
    ip.no_alloc = true;
    state_ctx_ = ggml_init(ip);
    if (!state_ctx_) {
        err = "ggml_init failed";
        return false;
    }
    std::vector<ggml_tensor *> twins;
    for (ggml_tensor * t : states) {
        if (!t->buffer || !t->data || t->view_src || !ggml_is_contiguous(t)) {
            err = std::string("state tensor ") + t->name + " is not an allocated contiguous leaf";
            return false;
        }
        ggml_tensor * s = ggml_dup_tensor(state_ctx_, t);
        ggml_set_name(s, t->name);
        twins.push_back(s);
    }
    state_buf_ = ggml_backend_alloc_ctx_tensors_from_buft(state_ctx_, buft);
    if (!state_buf_) {
        err = std::string("cannot allocate the model state in ") + where;
        return false;
    }
    ggml_backend_buffer_set_usage(state_buf_, ggml_backend_buffer_get_usage(states[0]->buffer));
    std::vector<uint8_t> tmp;
    for (size_t i = 0; i < states.size(); ++i) {
        ggml_tensor * t = states[i];
        ggml_tensor * s = twins[i];
        tmp.resize(ggml_nbytes(t));
        ggml_backend_tensor_get(t, tmp.data(), 0, tmp.size());
        ggml_backend_tensor_set(s, tmp.data(), 0, tmp.size());
        Entry e;
        e.t = t;
        e.host = {t->buffer, t->data, t->extra};
        e.dev = {s->buffer, s->data, s->extra};
        apply(t, e.dev);
        states_.push_back(e);
        state_bytes_ += ggml_nbytes(t);
    }
    return true;
}

void PrefillDevice::clear_state() {
    if (state_buf_) ggml_backend_buffer_clear(state_buf_, 0);
}

void PrefillDevice::apply(ggml_tensor * t, const Binding & b) {
    t->buffer = b.buffer;
    t->data = b.data;
    t->extra = b.extra;
}

void PrefillDevice::place(bool on_device) {
    if (on_device == on_device_) return;
    for (const Entry & e : entries_)
        apply(e.t, on_device ? e.dev : e.host);
    on_device_ = on_device;
}

bool PrefillDevice::init(ggml_backend_dev_t dev, const std::vector<ggml_tensor *> & weights, std::string & err) {
    if (!dev) {
        err = "no device";
        return false;
    }
    if (weights.empty()) return true; // everything goes through an arena, or the model has no layer weights
    for (ggml_tensor * w : weights) {
        if (!w->buffer || !ggml_backend_buffer_is_host(w->buffer) || !w->data) {
            err = std::string("weight ") + w->name + " is not host readable";
            return false;
        }
        if (!ggml_is_contiguous(w) || w->view_src) {
            err = std::string("weight ") + w->name + " is not a contiguous leaf";
            return false;
        }
    }

    ggml_init_params ip{};
    ip.mem_size = ggml_tensor_overhead() * (weights.size() + 1);
    ip.mem_buffer = nullptr;
    ip.no_alloc = true;
    ctx_ = ggml_init(ip);
    if (!ctx_) {
        err = "ggml_init failed";
        return false;
    }

    std::vector<ggml_tensor *> twins;
    twins.reserve(weights.size());
    for (ggml_tensor * w : weights) {
        ggml_tensor * s = ggml_dup_tensor(ctx_, w);
        ggml_set_name(s, w->name);
        twins.push_back(s);
    }

    ggml_backend_buffer_type_t buft = ggml_backend_dev_buffer_type(dev);
    buf_ = ggml_backend_alloc_ctx_tensors_from_buft(ctx_, buft);
    if (!buf_) {
        err = std::string("cannot allocate the layer weights on ") + ggml_backend_dev_name(dev);
        return false;
    }
    // Before the copy: a backend may choose the layout from the usage (Hexagon repacks WEIGHTS only),
    // and the scheduler's "run the op where the weight is" rule only looks at WEIGHTS buffers.
    ggml_backend_buffer_set_usage(buf_, GGML_BACKEND_BUFFER_USAGE_WEIGHTS);

    entries_.reserve(weights.size());
    for (size_t i = 0; i < weights.size(); ++i) {
        ggml_tensor * w = weights[i];
        ggml_tensor * s = twins[i];
        ggml_backend_tensor_set(s, w->data, 0, ggml_nbytes(w));
        Entry e;
        e.t = w;
        e.host = {w->buffer, w->data, w->extra};
        e.dev = {s->buffer, s->data, s->extra};
        entries_.push_back(e);
        bytes_ += ggml_nbytes(w);
    }
    return true;
}

} // namespace bmoe::detail
