#include "sim_device.h"

#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml.h"

#include <cstring>
#include <memory>

namespace bmoe {

namespace {

// The figures of a CUDA buffer the fitter's sums depend on (ggml-cuda.cu): a quantized tensor's last
// row padded to 512 elements, 128-byte alignment, and op offload from a 32-row batch.
constexpr int64_t k_row_padding = 512;
constexpr size_t k_alignment = 128;
constexpr int64_t k_offload_min_batch = 32;

struct SimDevice {
    std::string name, description;
    uint64_t total = 0, free = 0;
    ggml_backend_device dev{};
    ggml_backend_buffer_type buft{};
};

struct SimRegistry {
    std::vector<std::unique_ptr<SimDevice>> devices;
    ggml_backend_reg reg{};
};

ggml_backend_dev_t cpu_device() {
    return ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
}

ggml_backend_buffer_type_t cpu_buft() {
    return ggml_backend_dev_buffer_type(cpu_device());
}

SimDevice * of(ggml_backend_dev_t dev) {
    return (SimDevice *) dev->context;
}

// ── the buffer type: the card's memory, held in host memory when anything is really allocated ──

const char * buft_name(ggml_backend_buffer_type_t buft) {
    return ((SimDevice *) buft->context)->name.c_str();
}

ggml_backend_buffer_t buft_alloc(ggml_backend_buffer_type_t buft, size_t size) {
    // A plan allocates nothing (the fitter loads with no_alloc); were anything allocated, it is a
    // host buffer that reports this type, so the books stay this device's.
    ggml_backend_buffer_t b = ggml_backend_buft_alloc_buffer(cpu_buft(), size);
    if (b) b->buft = buft;
    return b;
}

size_t buft_alignment(ggml_backend_buffer_type_t) {
    return k_alignment;
}

size_t buft_max_size(ggml_backend_buffer_type_t) {
    return SIZE_MAX;
}

size_t buft_alloc_size(ggml_backend_buffer_type_t, const ggml_tensor * t) {
    size_t size = ggml_nbytes(t);
    if (ggml_is_quantized(t->type) && t->ne[0] % k_row_padding != 0)
        size += ggml_row_size(t->type, k_row_padding - t->ne[0] % k_row_padding);
    return size;
}

bool buft_is_host(ggml_backend_buffer_type_t) {
    return false;
}

// ── the backend: the CPU's, under this device's name ──

void backend_free(ggml_backend_t backend) {
    ggml_backend_t cpu = (ggml_backend_t) backend->context;
    ggml_backend_free(cpu);
    delete backend;
}

ggml_backend_t dev_init_backend(ggml_backend_dev_t dev, const char * params) {
    ggml_backend_t cpu = ggml_backend_dev_init(cpu_device(), params);
    if (!cpu) return nullptr;
    auto * b = new ggml_backend{};
    // A guid of its own: with the CPU's, ggml_backend_is_cpu() holds for this backend too, and llama.cpp
    // then reserves a second host compute buffer for it (measured: twice the host compute a CUDA load
    // reports).
    static ggml_guid sim_guid = {0x62, 0x6d, 0x6f, 0x65, 0x2d, 0x73, 0x69, 0x6d,
                                 0x2d, 0x64, 0x65, 0x76, 0x69, 0x63, 0x65, 0x00};
    b->guid = &sim_guid;
    b->iface = cpu->iface;
    b->iface.free = backend_free;
    // The CPU backend's functions read their own context; the wrapper keeps the CPU backend there
    // and forwards the calls a plan can make through a thin shim.
    b->iface.get_name = [](ggml_backend_t be) { return of(be->device)->name.c_str(); };
    b->iface.synchronize = nullptr;
    b->iface.graph_plan_create = nullptr;
    b->iface.graph_plan_free = nullptr;
    b->iface.graph_plan_update = nullptr;
    b->iface.graph_plan_compute = nullptr;
    b->iface.set_tensor_async = nullptr;
    b->iface.get_tensor_async = nullptr;
    b->iface.set_tensor_2d_async = nullptr;
    b->iface.get_tensor_2d_async = nullptr;
    b->iface.cpy_tensor_async = nullptr;
    b->iface.event_record = nullptr;
    b->iface.event_wait = nullptr;
    b->iface.graph_optimize = nullptr;
    b->iface.graph_compute = [](ggml_backend_t be, ggml_cgraph * g) {
        return ggml_backend_graph_compute((ggml_backend_t) be->context, g);
    };
    b->device = dev;
    b->context = cpu;
    return b;
}

// ── the device ──

const char * dev_name(ggml_backend_dev_t dev) {
    return of(dev)->name.c_str();
}

const char * dev_description(ggml_backend_dev_t dev) {
    return of(dev)->description.c_str();
}

void dev_memory(ggml_backend_dev_t dev, size_t * free, size_t * total) {
    *free = (size_t) of(dev)->free;
    *total = (size_t) of(dev)->total;
}

enum ggml_backend_dev_type dev_type(ggml_backend_dev_t) {
    return GGML_BACKEND_DEVICE_TYPE_GPU;
}

void dev_props(ggml_backend_dev_t dev, ggml_backend_dev_props * props) {
    props->name = dev_name(dev);
    props->description = dev_description(dev);
    dev_memory(dev, &props->memory_free, &props->memory_total);
    props->type = GGML_BACKEND_DEVICE_TYPE_GPU;
    props->device_id = dev_name(dev);
    props->caps = {}; // no async copies, no pinned host buffer, no host-pointer buffers, no events
}

ggml_backend_buffer_type_t dev_buft(ggml_backend_dev_t dev) {
    return &of(dev)->buft;
}

// Every op: a CUDA card computes every op a plan's graph holds, and an op refused here would be
// scheduled on the host, which moves compute buffers off the card the real fitter would have left
// there (measured: DeepSeek V4.1 on a 12 GB card, 5.5 GB of host compute and 7 layers too few while
// the simulated device forwarded the CPU's answer).
bool dev_supports_op(ggml_backend_dev_t, const ggml_tensor *) {
    return true;
}

bool dev_supports_buft(ggml_backend_dev_t dev, ggml_backend_buffer_type_t buft) {
    return buft == &of(dev)->buft;
}

int64_t op_batch_size(const ggml_tensor * op) {
    switch (op->op) {
    case GGML_OP_GET_ROWS:
        return 0;
    case GGML_OP_MUL_MAT:
        return op->ne[1];
    case GGML_OP_MUL_MAT_ID:
    case GGML_OP_ROPE:
    case GGML_OP_ROPE_BACK:
        return op->ne[2];
    default:
        return ggml_nrows(op);
    }
}

bool dev_offload_op(ggml_backend_dev_t, const ggml_tensor * op) {
    return op_batch_size(op) >= k_offload_min_batch;
}

// ── the registry ──

const char * reg_name(ggml_backend_reg_t) {
    return "SIM";
}

size_t reg_count(ggml_backend_reg_t reg) {
    return ((SimRegistry *) reg->context)->devices.size();
}

ggml_backend_dev_t reg_device(ggml_backend_reg_t reg, size_t i) {
    return &((SimRegistry *) reg->context)->devices.at(i)->dev;
}

void * reg_proc(ggml_backend_reg_t, const char *) {
    return nullptr;
}

SimRegistry * g_registry = nullptr;

} // namespace

std::vector<std::string> register_sim_devices(const std::vector<SimDeviceSpec> & specs) {
    std::vector<std::string> names;
    if (!g_registry) {
        g_registry = new SimRegistry{}; // lives as long as the backend registry that holds it
        for (size_t i = 0; i < specs.size(); ++i) {
            auto d = std::make_unique<SimDevice>();
            d->name = "SIM" + std::to_string(i);
            d->description = specs[i].description.empty() ? "simulated device" : specs[i].description;
            d->total = specs[i].memory_total;
            d->free = specs[i].memory_free ? specs[i].memory_free
                      : specs[i].memory_total > sim_driver_reserve_bytes
                          ? specs[i].memory_total - sim_driver_reserve_bytes
                          : 0;
            d->buft.iface = {buft_name,     buft_alloc,      nullptr, buft_alignment,
                             buft_max_size, buft_alloc_size, nullptr, buft_is_host};
            d->buft.device = &d->dev;
            d->buft.context = d.get();
            d->dev.iface = {dev_name,          dev_description, dev_memory, dev_type, dev_props,
                            dev_init_backend,  dev_buft,        nullptr,    nullptr,  dev_supports_op,
                            dev_supports_buft, dev_offload_op,  nullptr,    nullptr,  nullptr};
            d->dev.reg = &g_registry->reg;
            d->dev.context = d.get();
            g_registry->devices.push_back(std::move(d));
        }
        g_registry->reg.api_version = GGML_BACKEND_API_VERSION;
        g_registry->reg.iface = {reg_name, reg_count, reg_device, reg_proc};
        g_registry->reg.context = g_registry;
        ggml_backend_register(&g_registry->reg);
    }
    for (const auto & d : g_registry->devices)
        names.push_back(d->name);
    return names;
}

} // namespace bmoe
