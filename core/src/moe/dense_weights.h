// The dense (non-expert) model weights, and the policy for keeping them resident.
//
// The streamer rebinds only the expert tensors; everything else in the gguf — header/metadata,
// embeddings, attention, norms, lm_head — is "dense", and how it is kept in RAM is a policy of its
// own (DenseWeightsMode): left mmap'd, mmap'd-and-warmed, or read once into anonymous buffers via
// O_DIRECT and rebound. This module owns that policy, the buffers it may allocate, and the residency
// sensor that reports how much of the dense set the kernel still has — the half the expert cache's
// own residency sensor is blind to.
//
// It reads through its OWN FileReader, so its O_DIRECT choice is independent of the expert stream's:
// the dense set may be pulled cache-bypassing while the experts are not, or the reverse. There is one
// definition of "which bytes are dense" here (byte_ranges), shared by the warm sweep, the anonymous
// read, and the sensor — the three used to compute it separately.
#pragma once

#include "../io/file_reader.h"
#include "../io/platform_io.h"
#include "bmoe/config.h" // DenseWeightsMode

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ggml_tensor;

namespace bmoe {

// One dense weight tensor: where its bytes live in the gguf, and the tensor whose ->data we rebind.
// Read whole and contiguous (`size` bytes from `file_off`), unlike an expert slice.
struct DenseTensorRef {
    ggml_tensor * tensor = nullptr;
    uint64_t file_off = 0;
    uint64_t size = 0;
};

class DenseWeights {
public:
    DenseWeights() = default;
    ~DenseWeights();

    DenseWeights(const DenseWeights &) = delete;
    DenseWeights & operator=(const DenseWeights &) = delete;

    // The dense byte ranges: the complement of `expert_ranges` within [0, file_size). Sorts the
    // input; the result is the gaps between expert ranges plus the trailing tail. The single source
    // of truth every consumer here shares.
    static std::vector<std::pair<uint64_t, uint64_t>>
    byte_ranges(std::vector<std::pair<uint64_t, uint64_t>> expert_ranges, uint64_t file_size);

    // Apply `mode` for the model at `path` (dense `ranges` precomputed via byte_ranges; `tensors`
    // needed only for Anonymous). `align` is the O_DIRECT block size. Runs once at load, before the
    // streamer's workers start (Anonymous rebinds tensor->data on the caller's thread). Returns false
    // only on a hard Anonymous failure (alloc/read); Mmap and Warmed cannot fail.
    bool init(DenseWeightsMode mode,
              const std::string & path,
              size_t align,
              std::vector<std::pair<uint64_t, uint64_t>> ranges,
              std::vector<DenseTensorRef> tensors);

    // Sample how much of the dense set the kernel still has in RAM (mincore), setting resident_frac().
    // Anonymous samples the anon buffers; Mmap/Warmed sample the mmap ranges via /proc/self/maps.
    // The caller throttles (calls this only when it wants a fresh sample); `page` is the OS page size.
    void sample_residency(size_t page);
    double resident_frac() const { return resident_frac_; }

    void shutdown();

    static constexpr int sample_pages = 256; // stratified probe points across the dense bytes

private:
    bool read_anonymous(size_t align);
    void warm();
    void drop_mmap_copies(size_t page);
    void sample_anon(size_t page);
    void sample_mmap(size_t page);

    DenseWeightsMode mode_ = DenseWeightsMode::Warmed;
    std::string path_;
    std::string basename_;
    size_t align_ = 4096;

    // Anonymous/Pinned: our own reader, the tensors we read, and the buffers backing them. `bases_`
    // is what the sensor probes and is filled by both modes; `bufs_` and `pinned_` are the two
    // release lists, exactly one of which is populated for a given run.
    FileReader reader_;
    std::vector<DenseTensorRef> tensors_;
    std::vector<void *> bases_;
    std::vector<void *> bufs_;
    std::vector<pio::PinnedAlloc> pinned_;
    std::vector<size_t> buf_sz_;

    // Mmap/Warmed: the dense byte ranges and the mmap VMAs that back them, resolved once from
    // /proc/self/maps for the sensor.
    std::vector<std::pair<uint64_t, uint64_t>> ranges_;
    std::vector<pio::MappedRegion> vmas_;
    bool vmas_tried_ = false;

    double resident_frac_ = -1.0; // last sampled dense residency; -1 = never/unmeasured
};

} // namespace bmoe
