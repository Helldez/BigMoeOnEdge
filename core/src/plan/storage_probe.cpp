// The measured half of the machine profile: what this storage actually does with the reads this
// engine actually issues.
//
// Two facts come out of here, and neither can be inferred from a platform name:
//
//   * The rate curve. A 4 KiB read returns 2% of peak on one desktop SSD and 6.8% on a phone's
//     UFS, and lanes scale on one where they saturate on the other. The only honest source of a
//     lane count is therefore this machine, at this model's expert slice size.
//
//   * Whether a live mapping of the model serialises concurrent uncached reads. It is true on one
//     desktop filesystem and false on a phone's, and where it is true it costs most of the read
//     path's throughput. Measured by doing exactly that: read with a mapping alive, drop the
//     mapping, reopen the lanes, read again.
//
// The whole probe is bounded in time and reads a few tens of MiB. It runs only when asked.

#include "bmoe/probe.h"

#include "../io/file_reader.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bmoe {

namespace {

using clock_t_ = std::chrono::steady_clock;

// A live mapping of the model file, held only for as long as the probe needs to read against it.
// This is the one thing the probe cannot ask the OS about and has to reproduce.
class ScopedMapping {
public:
    explicit ScopedMapping(const char * path) { open(path); }
    ~ScopedMapping() { close(); }
    ScopedMapping(const ScopedMapping &) = delete;
    ScopedMapping & operator=(const ScopedMapping &) = delete;

    bool ok() const { return ok_; }
    void close();

private:
    void open(const char * path);
    bool ok_ = false;
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE section_ = nullptr;
    void * view_ = nullptr;
#else
    int fd_ = -1;
    void * addr_ = nullptr;
    size_t len_ = 0;
#endif
};

#if defined(_WIN32)
void ScopedMapping::open(const char * path) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wn <= 0) return;
    std::vector<wchar_t> w((size_t) wn);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w.data(), wn);
    file_ = CreateFileW(w.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return;
    section_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!section_) return;
    // Map the WHOLE file, the way a model loader does. A token-sized view is not equivalent: the
    // measured pathology tracks how much of the file is actually mapped and resident, so a probe
    // that maps a megabyte reproduces nothing and would report a confident "no".
    view_ = MapViewOfFile(section_, FILE_MAP_READ, 0, 0, 0);
    ok_ = view_ != nullptr;
}

void ScopedMapping::close() {
    if (view_) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (section_) {
        CloseHandle(section_);
        section_ = nullptr;
    }
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    ok_ = false;
}
#else
void ScopedMapping::open(const char * path) {
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) return;
    struct stat st {};
    if (::fstat(fd_, &st) != 0 || st.st_size <= 0) return;
    len_ = (size_t) st.st_size;
    addr_ = ::mmap(nullptr, len_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (addr_ == MAP_FAILED) {
        addr_ = nullptr;
        return;
    }
    ok_ = true;
}

void ScopedMapping::close() {
    if (addr_) {
        ::munmap(addr_, len_);
        addr_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    ok_ = false;
}
#endif

// Read `per_lane` blocks of `block` bytes per lane, at pseudo-random aligned offsets, and return
// the aggregate rate in MiB/s. Offsets are drawn from a stream seeded by the caller so two points
// of the curve do not read the same bytes and warm each other's drive cache.
double timed_read(FileReader & rd, uint64_t block, int lanes, int per_lane, uint64_t seed) {
    const uint64_t fsize = rd.file_size();
    if (fsize <= block * 4) return 0.0;
    const uint64_t span = fsize - block;

    std::vector<std::vector<char>> bufs((size_t) lanes);
    for (auto & b : bufs)
        b.resize((size_t) block);

    const auto t0 = clock_t_::now();
    std::vector<std::thread> ths;
    std::vector<long long> got((size_t) lanes, 0);
    ths.reserve((size_t) lanes);
    for (int l = 0; l < lanes; ++l) {
        ths.emplace_back([&, l] {
            uint64_t x = seed + (uint64_t) l * 0x9E3779B97F4A7C15ull;
            long long total = 0;
            for (int i = 0; i < per_lane; ++i) {
                // xorshift64*, so the offsets are spread without pulling in <random>
                x ^= x >> 12;
                x ^= x << 25;
                x ^= x >> 27;
                const uint64_t off = ((x * 0x2545F4914F6CDD1Dull) % span) & ~(uint64_t) 4095;
                const long long n = rd.read(l, bufs[(size_t) l].data(), off, block);
                if (n > 0) total += n;
            }
            got[(size_t) l] = total;
        });
    }
    for (auto & t : ths)
        t.join();
    const double secs = std::chrono::duration<double>(clock_t_::now() - t0).count();
    if (secs <= 0.0) return 0.0;

    long long bytes = 0;
    for (long long g : got)
        bytes += g;
    return (double) bytes / (1024.0 * 1024.0) / secs;
}

} // namespace

void probe_storage(HardwareProfile & hw, const char * model_path, uint64_t slice_bytes) {
    if (!model_path || !*model_path) return;

    // The sizes worth knowing are the ones around the read this engine issues. Below the model's
    // own slice is where a storage class shows whether it punishes small requests; above it is
    // where lanes stop paying.
    const uint64_t slice = slice_bytes ? slice_bytes : (512u << 10);
    const uint64_t sizes[3] = {std::max<uint64_t>(4096, slice / 4), slice, slice * 4};
    const int lane_counts[3] = {1, 2, 4};
    const int per_lane = 40; // per point: enough that the lane comparison is not decided by noise

    const size_t bounce = (size_t) sizes[2] + 65536;

    // Measure with a live mapping first, then release it and reopen the lanes. The order is the one
    // that matters on the storage where this bites: a handle opened while a section was alive keeps
    // serialising against it even after the section is gone, so the mapped case has to come first
    // and the unmapped case has to reopen.
    double mapped_rate = 0.0;
    {
        ScopedMapping map(model_path);
        if (map.ok()) {
            FileReader rd;
            if (rd.open(model_path, 4, /*direct=*/true, 4096, bounce)) {
                mapped_rate = timed_read(rd, slice, 4, per_lane, 0xC0FFEEull);
            }
        }
    }

    FileReader rd;
    if (!rd.open(model_path, 4, /*direct=*/true, 4096, bounce)) return;
    hw.storage.align = 4096;
    hw.storage.direct_ok = rd.direct() ? Tri::Yes : Tri::No;

    double unmapped_rate = 0.0;
    uint64_t seed = 0x1234567ull;
    for (uint64_t sz : sizes) {
        for (int lanes : lane_counts) {
            const double r = timed_read(rd, sz, lanes, per_lane, seed);
            seed += 0x9E3779B9ull;
            if (r <= 0.0) continue;
            hw.storage.rate_curve.push_back({(uint32_t) sz, (uint32_t) lanes, r});
            if (sz == slice && lanes == 4) unmapped_rate = r;
        }
    }

    // The verdict, from the two comparable points. The threshold is deliberately loose: what is
    // being detected is a collapse to roughly one lane's throughput, not a few percent of noise.
    hw.storage.rate_mapped_mibs = mapped_rate;
    hw.storage.rate_unmapped_mibs = unmapped_rate;

    // This probe reports Yes or nothing, never No, and the reason is a measurement that contradicted
    // it. On a desktop where the ENGINE gains 24% of decode from releasing the mapping (its own read
    // rate goes 871 -> 1680 MiB/s), this probe sees the two arms within 2% of each other. Whatever
    // the probe is failing to reproduce - most likely the access pattern, since it reads uniformly
    // at random where the engine walks expert slices layer by layer against a warm cache - its
    // fidelity is established in the positive direction only. A negative from an instrument that
    // missed a known positive is not evidence of absence, and reporting one as `measured` would be
    // the confident wrong answer this whole design exists to avoid.
    if (mapped_rate > 0.0 && unmapped_rate > 0.0 && mapped_rate < 0.7 * unmapped_rate) {
        hw.storage.mapping_serialises_reads = Tri::Yes;
    }
}

} // namespace bmoe
