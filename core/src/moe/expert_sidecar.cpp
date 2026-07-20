#include "bmoe/expert_sidecar.h"

#include "../io/platform_io.h"
#include "bmoe/recipe.h"
#include "gguf_offsets.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace bmoe {

namespace {

// Magic + format version fused: bump the trailing digits on any layout change so an old
// engine refuses a new file (and vice versa) instead of misreading it.
constexpr char sidecar_magic[8] = {'B', 'M', 'O', 'E', 'S', 'C', '0', '1'};
constexpr uint32_t sidecar_align = 4096;

// On-disk header, little-endian, fixed 48 bytes; the layer table follows immediately.
// Field-by-field serialized (no struct dump) so padding never leaks into the format.
struct DiskHeader {
    char magic[8];
    uint32_t align;
    uint32_t n_expert;
    uint32_t n_layers;
    uint32_t reserved;
    uint64_t source_size;
    uint64_t source_hash;
};
constexpr size_t disk_header_bytes = 8 + 4 + 4 + 4 + 4 + 8 + 8;
constexpr size_t disk_layer_bytes = 4 + 4 + 8 + 8 + 3 * 8; // il, pad, base, stride, proj[3]

uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

void put_u32(std::vector<uint8_t> & b, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back((uint8_t) (v >> (8 * i)));
}
void put_u64(std::vector<uint8_t> & b, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back((uint8_t) (v >> (8 * i)));
}
uint32_t get_u32(const uint8_t * p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= (uint32_t) p[i] << (8 * i);
    return v;
}
uint64_t get_u64(const uint8_t * p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (uint64_t) p[i] << (8 * i);
    return v;
}

// Discover the expert map of a gguf: every layer with at least one recipe-named expert
// tensor, its per-slot absolute offsets and per-expert slice sizes. This is the SAME walk
// the runtime's binding performs (recipe suffixes over `blk.<il>.<suffix>.weight`), so the
// two sides hash identical tuples. Offsets stay gguf-absolute here; entry offsets in the
// sidecar are assigned by the builder afterwards.
struct ExpertMap {
    bool ok = false;
    std::string error;
    uint32_t n_expert = 0;
    std::vector<SidecarLayer> layers;               // proj_bytes filled; base/stride unset
    std::vector<std::array<uint64_t, 3>> proj_offs; // parallel to layers: gguf offset per slot
};

ExpertMap discover_expert_map(const std::string & gguf_path) {
    ExpertMap m;
    const GgufModelInfo info = read_gguf_model_info(gguf_path.c_str());
    if (!info.ok) {
        m.error = "not a readable gguf: " + gguf_path;
        return m;
    }
    if (info.n_expert <= 0) {
        m.error = "not a MoE model (expert_count missing or 0)";
        return m;
    }
    const MoeRecipe * recipe = find_moe_recipe(info.arch.c_str());
    if (!recipe) {
        m.error = "architecture '" + info.arch + "' is not in the streaming registry";
        return m;
    }
    const GgufOffsets offs = read_gguf_offsets(gguf_path.c_str());
    if (!offs.ok) {
        m.error = "failed to read tensor offsets";
        return m;
    }
    m.n_expert = (uint32_t) info.n_expert;

    // Layers are probed by name until a gap of absent layers ends the scan; gguf layer
    // count is not read directly so this cannot drift from what the names actually say.
    for (uint32_t il = 0;; ++il) {
        SidecarLayer L;
        L.il = il;
        std::array<uint64_t, 3> po = {0, 0, 0};
        bool any = false, any_this_prefix = false;
        const std::string prefix = "blk." + std::to_string(il) + ".";
        // Cheap existence probe for the layer prefix: any recipe tensor OR attention norm.
        for (const auto & kv : offs.off_by_name) {
            if (kv.first.compare(0, prefix.size(), prefix) == 0) {
                any_this_prefix = true;
                break;
            }
        }
        if (!any_this_prefix) break;
        for (int p = 0; p < MoeRecipe::max_exps; ++p) {
            if (!recipe->exps_suffix[p]) continue;
            const std::string name = prefix + recipe->exps_suffix[p] + ".weight";
            auto io = offs.off_by_name.find(name);
            auto is = offs.size_by_name.find(name);
            if (io == offs.off_by_name.end() || is == offs.size_by_name.end()) continue;
            if (is->second % m.n_expert != 0) {
                m.error = "tensor " + name + " size is not divisible by expert count";
                return m;
            }
            po[(size_t) p] = io->second;
            L.proj_bytes[p] = is->second / m.n_expert;
            any = true;
        }
        if (!any) continue; // a dense / attention-only block in a hybrid stack: no row
        m.layers.push_back(L);
        m.proj_offs.push_back(po);
    }
    if (m.layers.empty()) {
        m.error = "no expert tensors found for architecture '" + info.arch + "'";
        return m;
    }
    m.ok = true;
    return m;
}

} // namespace

uint64_t expert_map_hash(uint32_t n_expert, const std::vector<SidecarLayer> & layers) {
    // FNV-1a 64. The hashed tuple stream is part of the format: change it only with the magic.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v, int nbytes) {
        for (int i = 0; i < nbytes; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 1099511628211ull;
        }
    };
    mix(n_expert, 4);
    for (const SidecarLayer & L : layers) {
        mix(L.il, 4);
        for (int p = 0; p < 3; ++p)
            mix(L.proj_bytes[p], 8);
    }
    return h;
}

std::string sidecar_path_for(const std::string & gguf_path) {
    return gguf_path + ".experts.bmoe";
}

bool build_expert_sidecar(const std::string & gguf_path,
                          const std::string & out_path,
                          std::string & error,
                          const std::function<void(uint64_t, uint64_t)> & progress) {
    ExpertMap map = discover_expert_map(gguf_path);
    if (!map.ok) {
        error = map.error;
        return false;
    }

    // Assign entry geometry: table right after the header, data page-aligned after it.
    const size_t table_bytes = disk_header_bytes + map.layers.size() * disk_layer_bytes;
    uint64_t cursor = align_up(table_bytes, sidecar_align);
    uint64_t payload = 0;
    for (SidecarLayer & L : map.layers) {
        uint64_t entry = 0;
        for (int p = 0; p < 3; ++p)
            entry += L.proj_bytes[p];
        L.base_off = cursor;
        L.entry_stride = align_up(entry, sidecar_align);
        cursor += L.entry_stride * map.n_expert;
        payload += entry * map.n_expert;
    }

    pio::fd_t src = pio::open_read(gguf_path.c_str(), false);
    if (!pio::fd_ok(src)) {
        error = "cannot open source gguf";
        return false;
    }
    const uint64_t src_size = pio::file_size(src);

    std::FILE * out = std::fopen(out_path.c_str(), "wb");
    if (!out) {
        pio::close_fd(src);
        error = "cannot create " + out_path;
        return false;
    }

    // Serialize the header with a ZEROED magic first; the real magic goes in only after
    // every byte is written and flushed, so a torn build never validates.
    std::vector<uint8_t> head;
    head.reserve(table_bytes);
    for (int i = 0; i < 8; ++i)
        head.push_back(0);
    put_u32(head, sidecar_align);
    put_u32(head, map.n_expert);
    put_u32(head, (uint32_t) map.layers.size());
    put_u32(head, 0);
    put_u64(head, src_size);
    put_u64(head, expert_map_hash(map.n_expert, map.layers));
    for (const SidecarLayer & L : map.layers) {
        put_u32(head, L.il);
        put_u32(head, 0);
        put_u64(head, L.base_off);
        put_u64(head, L.entry_stride);
        for (int p = 0; p < 3; ++p)
            put_u64(head, L.proj_bytes[p]);
    }

    auto fail = [&](const std::string & msg) {
        std::fclose(out);
        pio::close_fd(src);
        std::remove(out_path.c_str());
        error = msg;
        return false;
    };

    if (std::fwrite(head.data(), 1, head.size(), out) != head.size()) return fail("header write failed");

    // Stream the slices across, layer-major then expert-major — the exact read order the
    // engine will use, which also makes the copy itself sequential per projection stride.
    std::vector<char> buf(8u << 20);
    uint64_t done = 0;
    for (size_t li = 0; li < map.layers.size(); ++li) {
        const SidecarLayer & L = map.layers[li];
        for (uint32_t e = 0; e < map.n_expert; ++e) {
            uint64_t w = L.base_off + (uint64_t) e * L.entry_stride;
#if defined(_WIN32)
            _fseeki64(out, (long long) w, SEEK_SET);
#else
            fseeko(out, (off_t) w, SEEK_SET);
#endif
            for (int p = 0; p < 3; ++p) {
                const uint64_t sz = L.proj_bytes[p];
                if (sz == 0) continue;
                uint64_t roff = map.proj_offs[li][(size_t) p] + (uint64_t) e * sz;
                uint64_t left = sz;
                while (left) {
                    const size_t chunk = (size_t) std::min<uint64_t>(left, buf.size());
                    const long long got = pio::pread_at(src, buf.data(), chunk, roff);
                    if (got <= 0) return fail("read failed in source gguf");
                    if (std::fwrite(buf.data(), 1, (size_t) got, out) != (size_t) got)
                        return fail("write failed (disk full?)");
                    roff += (uint64_t) got;
                    left -= (uint64_t) got;
                    done += (uint64_t) got;
                }
            }
            if (progress && (e % 16 == 0)) progress(done, payload);
        }
    }
    if (progress) progress(payload, payload);

    if (std::fflush(out) != 0) return fail("flush failed");
    // Magic last: rewrite the first 8 bytes now that the payload is durable in the file.
    if (std::fseek(out, 0, SEEK_SET) != 0) return fail("seek failed");
    if (std::fwrite(sidecar_magic, 1, 8, out) != 8) return fail("magic write failed");
    if (std::fflush(out) != 0) return fail("final flush failed");
    std::fclose(out);
    pio::close_fd(src);
    return true;
}

SidecarIndex load_sidecar_index(const std::string & path) {
    SidecarIndex idx;
    pio::fd_t fd = pio::open_read(path.c_str(), false);
    if (!pio::fd_ok(fd)) {
        idx.error = "cannot open " + path;
        return idx;
    }
    const uint64_t fsize = pio::file_size(fd);
    uint8_t h[disk_header_bytes];
    if (pio::pread_at(fd, h, sizeof(h), 0) != (long long) sizeof(h)) {
        pio::close_fd(fd);
        idx.error = "truncated header";
        return idx;
    }
    if (std::memcmp(h, sidecar_magic, 8) != 0) {
        pio::close_fd(fd);
        idx.error = "bad magic (incomplete build or wrong format version)";
        return idx;
    }
    idx.align = get_u32(h + 8);
    idx.n_expert = get_u32(h + 12);
    const uint32_t n_layers = get_u32(h + 16);
    idx.source_size = get_u64(h + 24);
    idx.source_hash = get_u64(h + 32);
    if (idx.align == 0 || idx.n_expert == 0 || n_layers == 0 || n_layers > 4096) {
        pio::close_fd(fd);
        idx.error = "implausible header fields";
        return idx;
    }
    std::vector<uint8_t> t(n_layers * disk_layer_bytes);
    if (pio::pread_at(fd, t.data(), t.size(), disk_header_bytes) != (long long) t.size()) {
        pio::close_fd(fd);
        idx.error = "truncated layer table";
        return idx;
    }
    idx.layers.resize(n_layers);
    for (uint32_t i = 0; i < n_layers; ++i) {
        const uint8_t * r = t.data() + (size_t) i * disk_layer_bytes;
        SidecarLayer & L = idx.layers[i];
        L.il = get_u32(r);
        L.base_off = get_u64(r + 8);
        L.entry_stride = get_u64(r + 16);
        uint64_t entry = 0;
        for (int p = 0; p < 3; ++p) {
            L.proj_bytes[p] = get_u64(r + 24 + 8 * p);
            entry += L.proj_bytes[p];
        }
        // Structural sanity: every entry must lie inside the file.
        if (L.entry_stride < entry || L.base_off + (uint64_t) idx.n_expert * L.entry_stride > fsize) {
            pio::close_fd(fd);
            idx.error = "layer table inconsistent with file size";
            idx.layers.clear();
            return idx;
        }
    }
    pio::close_fd(fd);
    idx.ok = true;
    return idx;
}

} // namespace bmoe
