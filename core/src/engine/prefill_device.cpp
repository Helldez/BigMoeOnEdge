#include "prefill_device.h"

#include "ggml-alloc.h"
#include "ggml.h"

namespace bmoe::detail {

PrefillDevice::~PrefillDevice() {
    // The model outlives nothing it cannot read: hand every weight back to its mapping before the
    // device copy it may still point at is freed.
    to_host();
    if (buf_) ggml_backend_buffer_free(buf_);
    if (ctx_) ggml_free(ctx_);
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
    if (weights.empty()) {
        err = "the graph read no layer weights";
        return false;
    }
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
