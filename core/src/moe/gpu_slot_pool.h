// A residency pool for routed experts held in GPU memory.
//
// The streaming path on the CPU works by rebinding an expert tensor's `->data` to a host buffer the
// engine refills. A device buffer has no host address to rebind, so the GPU needs the other shape
// of the same idea: the expert tensors are allocated with only `n_slots` experts along dim 2 (see
// `llama_model_params::moe_expert_slots`), and the engine keeps deciding which experts occupy those
// slots, writing a slice at a time.
//
// The routing still names global expert ids, so every dispatch has to be told which slot holds the
// expert it asked for. That is the whole job of the hook installed here: read the ids the router
// produced, make the experts they name resident, and hand the dispatch a rewritten id vector.
//
// This is where the design pays its one unavoidable cost. Only the host can read from flash, so the
// GPU pipeline must stall once per MoE layer while the host inspects the routing - the same stall
// that made this approach lose on other unified-memory backends. Whether it is affordable is a
// measurement, not an assumption, which is why `stats()` reports it separately.
#pragma once

#include "bmoe/recipe.h"
#include "expert_stream_source.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

struct ggml_tensor;
struct ggml_context;
struct ggml_backend_buffer;
struct ggml_opencl_moe_slots; // the backend's substitute pair; declared here to avoid the header

namespace bmoe {

class FileReader;

struct GpuSlotStats {
    uint64_t lookups = 0;   // hook invocations that named a pooled tensor
    uint64_t readbacks = 0; // id vectors fetched back from the device (the per-layer stall)
    uint64_t hits = 0;      // experts already resident
    uint64_t misses = 0;    // experts uploaded
    uint64_t bytes_up = 0;  // bytes written into slots
    uint64_t stall_ns = 0;  // time spent inside the hook, i.e. with the GPU queue drained
    uint64_t read_ns = 0;   // of stall_ns: flash reads (misses only)
    uint64_t upload_ns = 0; // of stall_ns: staging write + convert kernel (misses only)
};

class GpuSlotPool final {
public:
    GpuSlotPool() = default;
    ~GpuSlotPool();

    GpuSlotPool(const GpuSlotPool &) = delete;
    GpuSlotPool & operator=(const GpuSlotPool &) = delete;

    // `layers` must carry, for every MoE layer, the expert weight tensors as the model allocated
    // them (already narrowed to `n_slots` along dim 2) together with their gguf offsets. `reader`
    // supplies the bytes and must outlive the pool. Returns false with `error()` set if the build
    // has no OpenCL slot hook, or if the tensors were not narrowed - streaming into a full-size
    // tensor would silently address the wrong expert rather than fail.
    bool init(std::vector<LayerExperts> layers, int n_expert, int n_slots, FileReader * reader);

    // Installs a hook that only keeps the routing in range, for the capture decode that has to run
    // before the expert tensors are even known. Without it that decode indexes a narrowed tensor
    // with the model's full expert ids and reads past the pool. Its output is arithmetically
    // meaningless, which is fine: the capture pass exists to harvest tensor pointers and its KV is
    // discarded.
    void arm_for_capture(int n_slots);

    // Route every MUL_MAT_ID over a pooled tensor through this pool. Process-global, like the
    // backend hook it wraps; only one pool may be installed at a time.
    void install();
    void uninstall();

    const GpuSlotStats & stats() const { return stats_; }
    const std::string & error() const { return error_; }

private:
    static bool
    c_hook(const ggml_tensor * weights, const ggml_tensor * ids, struct ggml_opencl_moe_slots * out, void * user_data);
    bool on_hook(const ggml_tensor * weights, const ggml_tensor * ids, struct ggml_opencl_moe_slots * out);

    // Makes `expert` resident in some slot of layer `il`, uploading every projection on a miss.
    // Returns the slot index, or -1 if a read or an upload failed.
    int ensure_resident(int il, int expert);

    struct LayerSlots {
        std::vector<int> slot_expert;             // slot -> expert id, -1 when free
        std::vector<int> slot_lru;                // slot indices, least recently used first
        std::unordered_map<int, int> expert_slot; // expert id -> slot
    };

    // The backend recovers the expert count from the routing tensor's ROW STRIDE, not from its
    // width, because in the graph the routing is a view into a wider allocation. A substitute must
    // reproduce that: `base` is n_slots wide, `view` is the k-wide window the dispatch reads, and
    // writes go to `base` at the full stride.
    struct IdsBuffer {
        int64_t ne0 = 0;
        int64_t ne1 = 0;
        ggml_context * ctx = nullptr;
        ggml_backend_buffer * buf = nullptr;
        ggml_tensor * base = nullptr;
        ggml_tensor * view = nullptr;
    };

    IdsBuffer * ids_buffer(const ggml_tensor * like, int64_t ne0, int64_t ne1);

    std::vector<LayerExperts> layers_;
    std::vector<LayerSlots> slots_;
    std::unordered_map<const ggml_tensor *, int> tensor_layer_; // pooled tensor -> layer index
    std::vector<IdsBuffer> ids_bufs_;

    std::vector<uint8_t> staging_;
    std::vector<int32_t> host_ids_;
    std::vector<int32_t> mapped_ids_;

    // The three projections of a layer are dispatched back to back against the same routing, so the
    // ids only have to be fetched once per layer. Keyed by the routing tensor as well as the layer
    // so a graph that reuses a layer index cannot inherit a stale mapping.
    int cached_layer_ = -1;
    const ggml_tensor * cached_ids_ = nullptr;

    FileReader * reader_ = nullptr;
    int n_expert_ = 0;
    int n_slots_ = 0;
    bool capturing_ = false;
    bool installed_ = false;
    GpuSlotStats stats_;
    std::string error_;
};

} // namespace bmoe
