#include "sim_parts.h"

#include "ggml.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bmoe {

namespace {

constexpr double k_gib = 1024.0 * 1024.0 * 1024.0;
constexpr double k_gb = 1e9;

bool read_file(const std::string & path, std::string & text) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream all;
    all << f.rdbuf();
    text = all.str();
    return true;
}

// "8G" -> bytes; a bare number is GB.
uint64_t size_of(const std::string & s) {
    char * end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    const std::string u = end ? end : "";
    const double mul = u == "M" ? 1024.0 * 1024.0 : 1024.0 * 1024.0 * 1024.0;
    return (uint64_t) std::llround(v * mul);
}

// key=value words, a value in double quotes may hold spaces.
std::map<std::string, std::string> words_of(const std::string & line, std::vector<std::string> & bare) {
    std::map<std::string, std::string> kv;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace((unsigned char) line[i]))
            ++i;
        if (i >= line.size()) break;
        size_t j = i;
        std::string key, value;
        bool has_eq = false;
        while (j < line.size() && !std::isspace((unsigned char) line[j]) && line[j] != '=')
            key += line[j++];
        if (j < line.size() && line[j] == '=') {
            has_eq = true;
            ++j;
            if (j < line.size() && line[j] == '"') {
                ++j;
                while (j < line.size() && line[j] != '"')
                    value += line[j++];
                ++j;
            } else {
                while (j < line.size() && !std::isspace((unsigned char) line[j]))
                    value += line[j++];
            }
        }
        if (has_eq)
            kv[key] = value;
        else
            bare.push_back(key);
        i = j;
    }
    return kv;
}

double num(const std::map<std::string, std::string> & kv, const char * k, double dv = 0.0) {
    const auto it = kv.find(k);
    return it == kv.end() ? dv : std::atof(it->second.c_str());
}

// Samples of one term, one list per measured machine: a machine recorded on six models is still one
// machine, and the fit takes each machine's median before the median over machines.
struct Samples {
    std::map<std::string, std::vector<double>> by_machine;
    void add(const std::string & machine, double v) { by_machine[machine].push_back(v); }
    std::vector<double> per_machine() const;
};

Term median_term(std::vector<double> v) {
    Term t;
    if (v.empty()) return t;
    std::sort(v.begin(), v.end());
    t.n = (int) v.size();
    t.value = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    if (t.value > 0.0) {
        t.lo = v.front() / t.value;
        t.hi = v.back() / t.value;
    }
    return t;
}

std::vector<double> Samples::per_machine() const {
    std::vector<double> out;
    for (const auto & [machine, v] : by_machine)
        if (!v.empty()) out.push_back(median_term(v).value);
    return out;
}

// The layer's bytes a probe of the model's own block reads: a token's routed experts in one MoE
// layer, and a token's dense weights in one layer.
double expert_layer_bytes(const ModelProfile & m) {
    const uint32_t n = m.n_moe_layer ? m.n_moe_layer : m.n_layer;
    return n ? (double) m.group(WeightGroup::Experts).bytes_per_token / n : 0.0;
}

double dense_layer_bytes(const ModelProfile & m) {
    if (m.n_layer == 0) return 0.0;
    return ((double) m.group(WeightGroup::Attention).bytes_per_token +
            (double) m.group(WeightGroup::DenseFfn).bytes_per_token +
            (double) m.group(WeightGroup::Other).bytes_per_token) /
           m.n_layer;
}

double one_thread(const HardwareProfile & hw) {
    for (const auto & r : hw.thread_rates)
        if (r.threads == 1) return r.gibs;
    return 0.0;
}

const ComputeDevice * first_card(const HardwareProfile & hw) {
    for (const ComputeDevice & d : hw.devices)
        if (!d.is_cpu) return &d;
    return nullptr;
}

// Bits per weight of a ggml type, 0 when unknown.
double bits_per_weight(int type) {
    if (type < 0 || type >= GGML_TYPE_COUNT) return 0.0;
    const int64_t blck = ggml_blck_size((ggml_type) type);
    return blck > 0 ? 8.0 * (double) ggml_type_size((ggml_type) type) / (double) blck : 0.0;
}

// A host computes a quantization of 4 bits or more as fast as its memory feeds it, and one under 4
// bits as fast as its cores dequantize it: every measured host says so (memory-bound plateaus at
// 0.73-0.88 of the memory's specification on Q4 and Q8; 2-bit plateaus at 0.24-0.46 on servers and
// 0.42 on a laptop, under their cores' sum).
constexpr double k_compute_bound_bits = 4.0;
// A pageable upload under this share of the same link's pinned upload is bounded by the host's copy.
constexpr double k_copy_bound_of_pinned = 0.5;

std::string quant_name(int type) {
    return type >= 0 && type < GGML_TYPE_COUNT ? ggml_type_name((ggml_type) type) : "unknown";
}

// time = bytes / (e x bw) + t0, fitted on (bytes/bw, bytes/rate) by least squares; spread is
// measured / modelled rate.
void fit_layer(const std::vector<std::array<double, 3>> & pts, Term & eff, Term & lat) {
    // pts: bytes, spec bw (B/s), measured rate (B/s)
    if (pts.empty()) return;
    double e = 0.0, t0 = 0.0;
    if (pts.size() >= 2) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (const auto & p : pts) {
            const double x = p[0] / p[1], y = p[0] / p[2];
            sx += x, sy += y, sxx += x * x, sxy += x * y;
        }
        const double n = (double) pts.size(), den = n * sxx - sx * sx;
        const double a = den != 0.0 ? (n * sxy - sx * sy) / den : 0.0;
        t0 = (sy - a * sx) / n;
        e = a > 0.0 ? 1.0 / a : 0.0;
    }
    if (e <= 0.0 || t0 < 0.0) { // one machine, or a fit with no meaning: bandwidth alone
        std::vector<double> r;
        for (const auto & p : pts)
            r.push_back(p[2] / p[1]);
        e = median_term(r).value;
        t0 = 0.0;
    }
    std::vector<double> ratio;
    for (const auto & p : pts)
        ratio.push_back(p[2] / (p[0] / (p[0] / (e * p[1]) + t0)));
    Term spread = median_term(ratio);
    eff = {e, spread.lo * spread.value, spread.hi * spread.value, (int) pts.size()};
    lat = {t0, 1.0, 1.0, (int) pts.size()};
}

double layer_rate(double bytes, double bw_bytes, const Term & eff, const Term & lat) {
    if (bytes <= 0.0 || bw_bytes <= 0.0 || eff.value <= 0.0) return 0.0;
    return bytes / (bytes / (eff.value * bw_bytes) + lat.value);
}

std::string pct_range(const Term & t) {
    char buf[64];
    if (t.n <= 1)
        std::snprintf(buf, sizeof(buf), "one machine");
    else
        std::snprintf(buf, sizeof(buf), "%+.0f%%..%+.0f%% over %d machines", 100.0 * (t.lo - 1.0), 100.0 * (t.hi - 1.0),
                      t.n);
    return buf;
}

} // namespace

double link_gbs(const std::string & s) {
    unsigned gen = 0, lanes = 0;
    if (std::sscanf(s.c_str(), "pcie%ux%u", &gen, &lanes) != 2) return std::atof(s.c_str());
    // usable GB/s per lane after line coding
    const double per_lane = gen <= 2 ? 0.5 * gen : gen == 3 ? 0.985 : gen == 4 ? 1.969 : gen == 5 ? 3.938 : 7.877;
    return per_lane * lanes;
}

bool parse_parts_catalog(const std::string & text,
                         const std::string & base_dir,
                         PartsCatalog & out,
                         std::string & error) {
    std::istringstream lines(text);
    std::string line;
    int n = 0;
    while (std::getline(lines, line)) {
        ++n;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::vector<std::string> bare;
        const auto kv = words_of(line, bare);
        if (bare.empty()) continue;
        const std::string kind = bare[0];
        const std::string id = bare.size() > 1 ? bare[1] : "";
        auto get = [&](const char * k) {
            const auto it = kv.find(k);
            return it == kv.end() ? std::string() : it->second;
        };
        if (kind == "records") {
            out.records_dir =
                (std::filesystem::u8path(base_dir) / std::filesystem::u8path(id)).lexically_normal().u8string();
        } else if (kind == "gpu") {
            GpuSpec g;
            g.id = id;
            g.description = get("desc");
            g.vram = size_of(get("vram"));
            g.bw = num(kv, "bw");
            g.fp32 = num(kv, "fp32");
            g.link = link_gbs(get("link"));
            out.gpus[id] = g;
        } else if (kind == "cpu") {
            CpuSpec c;
            c.id = id;
            c.description = get("desc");
            c.cores = (uint32_t) num(kv, "cores");
            c.ghz = num(kv, "ghz");
            c.membw = num(kv, "membw");
            c.core = get("core");
            c.simd = get("simd");
            out.cpus[id] = c;
        } else if (kind == "rig") {
            Rig r;
            r.record = id;
            r.gpu = get("gpu");
            r.cpu = get("cpu");
            r.link = link_gbs(get("link"));
            out.rigs.push_back(r);
        } else {
            error = "line " + std::to_string(n) + ": unknown kind '" + kind + "'";
            return false;
        }
    }
    return true;
}

bool fit_parts_model(const PartsCatalog & cat, PartsModel & pm, std::string & error, const std::string & skip) {
    // Leaving a record out leaves its machine out: the same parts measured again on another model
    // would otherwise stand in for it, and the check would read better than the model is.
    std::string skip_gpu, skip_cpu;
    for (const Rig & r : cat.rigs)
        if (r.record == skip) skip_gpu = r.gpu, skip_cpu = r.cpu;
    std::vector<std::array<double, 3>> ex, de;
    Samples probe, pinned, pageable, pageable_cap, reported, split, hop, wide, plateau_mem, par_eff, ex_pl, de_pl,
        wi_pl;
    std::map<std::string, std::vector<double>> r1, low_bit;
    struct Obs {
        std::string core;
        int quant;
        double y; // log(r1 / ghz)
    };
    std::vector<Obs> obs;
    double best_storage = 0.0;
    bool have_host = false, have_card = false;
    for (const Rig & rig : cat.rigs) {
        if (rig.record == skip || (!skip.empty() && rig.gpu == skip_gpu && rig.cpu == skip_cpu)) continue;
        const auto cpu = cat.cpus.find(rig.cpu);
        if (cpu == cat.cpus.end()) continue;
        std::string text, err;
        PlanRecord rec;
        const std::string path = (std::filesystem::u8path(cat.records_dir) / (rig.record + ".planrec")).u8string();
        if (!read_file(path, text) || !parse_plan_record(text, rec, err)) continue;
        pm.fitted_on.push_back(rig.record);
        const HardwareProfile & hw = rec.hw;
        const ModelProfile & m = rec.model;
        // ── the host ──
        const CpuSpec & c = cpu->second;
        const std::string card_key = rig.gpu + "|" + rig.cpu + "|" + std::to_string(rig.link);
        const std::string host_key = card_key + "|" + std::to_string(m.expert_type_id);
        const double one = one_thread(hw), plateau = hw.host_bandwidth_gibs;
        if (one > 0.0 && c.ghz > 0.0) {
            r1[c.core + "|" + std::to_string(m.expert_type_id)].push_back(one / c.ghz);
            obs.push_back({c.core, m.expert_type_id, std::log(one / c.ghz)});
            if (bits_per_weight(m.expert_type_id) < k_compute_bound_bits) {
                low_bit[c.simd].push_back(one / c.ghz);
                low_bit["*"].push_back(one / c.ghz);
            }
        }
        if (plateau > 0.0 && c.membw > 0.0) {
            const double of_mem = plateau * k_gib / (c.membw * k_gb);
            // A plateau well under the memory's is the cores' limit, not the memory's.
            if (of_mem >= 0.6)
                plateau_mem.add(host_key, of_mem);
            else if (one > 0.0 && hw.n_cores > 0)
                par_eff.add(host_key, plateau / (one * hw.n_cores));
            if (hw.host_expert_gibs > 0.0) ex_pl.add(host_key, hw.host_expert_gibs / plateau);
            if (hw.host_dense_gibs > 0.0) de_pl.add(host_key, hw.host_dense_gibs / plateau);
            if (hw.host_wide_matmul_gibs > 0.0) wi_pl.add(host_key, hw.host_wide_matmul_gibs / plateau);
        }
        if (!have_host) {
            pm.host_template = rec;
            have_host = true;
        }
        if (hw.storage.rate_unmapped_mibs > best_storage && !hw.storage.rate_curve.empty()) {
            best_storage = hw.storage.rate_unmapped_mibs;
            double peak = 0.0;
            for (const RateSample & s : hw.storage.rate_curve)
                peak = std::max(peak, s.mib_per_s);
            pm.storage_shape = hw.storage.rate_curve;
            for (RateSample & s : pm.storage_shape)
                s.mib_per_s /= peak;
            pm.host_template.hw.storage = hw.storage;
        }
        // ── the card ──
        const auto gpu = cat.gpus.find(rig.gpu);
        const ComputeDevice * d = first_card(hw);
        if (gpu == cat.gpus.end() || !d || !d->has_own_memory()) continue;
        const GpuSpec & g = gpu->second;
        const double bw = g.bw * k_gb;
        if (d->expert_gibs > 0.0) ex.push_back({expert_layer_bytes(m), bw, d->expert_gibs * k_gib});
        if (d->dense_gibs > 0.0) de.push_back({dense_layer_bytes(m), bw, d->dense_gibs * k_gib});
        if (d->memory_bandwidth_gibs > 0.0) probe.add(card_key, d->memory_bandwidth_gibs * k_gib / bw);
        if (g.vram > 0 && d->memory_total > 0) reported.add(card_key, (double) d->memory_total / (double) g.vram);
        if (rig.link > 0.0) {
            if (d->upload_host_buffer_gibs > 0.0)
                pinned.add(card_key, d->upload_host_buffer_gibs * k_gib / (rig.link * k_gb));
            // A copy from pageable memory goes through the driver's staging buffer. Where it runs at
            // under half of what the same link carries from pinned memory, the host's copy is what
            // bounds it, not the link: on fast links it stays near the same rate whatever the link.
            if (d->upload_gibs > 0.0) {
                if (d->upload_host_buffer_gibs > 0.0 &&
                    d->upload_gibs < k_copy_bound_of_pinned * d->upload_host_buffer_gibs)
                    pageable_cap.add(card_key, d->upload_gibs);
                else if (d->upload_host_buffer_gibs > 0.0)
                    pageable.add(card_key, d->upload_gibs / d->upload_host_buffer_gibs);
            }
        }
        if (d->split_seconds > 0.0) split.add(card_key, d->split_seconds);
        if (d->host_hop_seconds > 0.0) hop.add(card_key, d->host_hop_seconds);
        if (d->wide_matmul_gibs > 0.0 && g.fp32 > 0.0) wide.add(card_key, d->wide_matmul_gibs / g.fp32);
        if (!have_card) {
            pm.device_template = *d;
            have_card = true;
        }
    }
    if (!have_host) {
        error = "no rig of the parts catalog has a record that reads under " + cat.records_dir;
        return false;
    }
    fit_layer(ex, pm.gpu_expert_eff, pm.gpu_expert_lat_s);
    fit_layer(de, pm.gpu_dense_eff, pm.gpu_dense_lat_s);
    pm.gpu_probe_bw = median_term(probe.per_machine());
    pm.gpu_pinned = median_term(pinned.per_machine());
    pm.gpu_pageable = median_term(pageable.per_machine());
    pm.gpu_pageable_cap_gibs = median_term(pageable_cap.per_machine());
    pm.gpu_reported_memory = median_term(reported.per_machine());
    pm.gpu_split_s = median_term(split.per_machine());
    pm.gpu_host_hop_s = median_term(hop.per_machine());
    pm.gpu_wide_per_tflop = median_term(wide.per_machine());
    for (const auto & [k, v] : r1)
        pm.r1_per_ghz[k] = median_term(v);
    for (const auto & [simd, v] : low_bit)
        pm.low_bit_r1_per_ghz[simd] = median_term(v);
    pm.host_plateau_of_membw = median_term(plateau_mem.per_machine());
    pm.host_parallel_eff = median_term(par_eff.per_machine());
    pm.host_expert_of_plateau = median_term(ex_pl.per_machine());
    pm.host_dense_of_plateau = median_term(de_pl.per_machine());
    pm.host_wide_of_plateau = median_term(wi_pl.per_machine());
    // log(r1/GHz) = core + quant, alternating least squares with a small pull to 0 so classes that
    // share no quantization with the rest still get a value (and are said to be apart).
    std::map<std::string, double> k;
    std::map<int, double> q;
    for (const Obs & o : obs)
        k[o.core] = 0.0, q[o.quant] = 0.0;
    for (int it = 0; it < 200; ++it) {
        for (auto & [core, kv] : k) {
            double s = 0, n = 0.1;
            for (const Obs & o : obs)
                if (o.core == core) s += o.y - q[o.quant], n += 1;
            kv = s / n;
        }
        for (auto & [qt, qv] : q) {
            double s = 0, n = 0.1;
            for (const Obs & o : obs)
                if (o.quant == qt) s += o.y - k[o.core], n += 1;
            qv = s / n;
        }
    }
    for (const auto & [core, v] : k)
        pm.core_k[core] = {v, 1, 1, 0};
    for (const auto & [qt, v] : q)
        pm.quant_q[qt] = {v, 1, 1, 0};
    return true;
}

HardwareProfile build_hardware(const MachineParts & parts,
                               const PartsModel & pm,
                               const ModelProfile & model,
                               std::vector<std::string> & notes) {
    HardwareProfile hw = pm.host_template.hw;
    hw.devices.clear();
    // ── memory ──
    hw.memory_total = parts.ram;
    hw.residency_budget = parts.available            ? parts.available
                          : parts.ram > (3ull << 30) ? parts.ram - (3ull << 30)
                                                     : parts.ram;
    hw.holdable_bytes = 0;
    hw.lockable_bytes = 0;
    // ── the host's matmul ──
    const CpuSpec & c = parts.cpu;
    const std::string key = c.core + "|" + std::to_string(model.expert_type_id);
    const double mem_plateau = pm.host_plateau_of_membw.value * c.membw * k_gb / k_gib;
    const double pe = pm.host_parallel_eff.value > 0.0 ? pm.host_parallel_eff.value : 0.7;
    const uint32_t cores = std::max<uint32_t>(1, c.cores);
    const double bits = bits_per_weight(model.expert_type_id);
    const bool compute_bound = bits > 0.0 && bits < k_compute_bound_bits;
    double r1 = 0.0;
    const auto exact = pm.r1_per_ghz.find(key);
    if (c.thread_gibs > 0.0) {
        r1 = c.thread_gibs;
        notes.push_back("one thread at the " + std::to_string(c.thread_gibs).substr(0, 5) +
                        " GiB/s the machine spec gives (thread=)");
    } else if (exact != pm.r1_per_ghz.end()) {
        r1 = exact->second.value * c.ghz;
        notes.push_back("one thread of " + c.core + " on " + quant_name(model.expert_type_id) +
                        ": measured on that class and quantization");
    } else if (compute_bound) {
        const auto same = pm.low_bit_r1_per_ghz.find(c.simd);
        const auto any = pm.low_bit_r1_per_ghz.find("*");
        const Term & t = same != pm.low_bit_r1_per_ghz.end() ? same->second : any->second;
        r1 = t.value * c.ghz;
        notes.push_back("one thread of " + c.core + " on " + quant_name(model.expert_type_id) + " (" +
                        std::to_string(bits).substr(0, 4) +
                        " bits a weight: its cores, not its memory, bound it): never measured; the median of the "
                        "hosts measured on a quantization under 4 bits" +
                        (same != pm.low_bit_r1_per_ghz.end() ? " with " + c.simd : std::string(" (any SIMD)")) + ", " +
                        pct_range(t) + ". Read the host's rates as -50%..+100%");
    } else {
        // Four bits or more: the memory bounds it, and one thread only shapes the sweep.
        r1 = mem_plateau / (cores * pe) * 2.0;
        notes.push_back("the host on " + quant_name(model.expert_type_id) + " (" + std::to_string(bits).substr(0, 4) +
                        " bits a weight) is bound by its memory, as every host measured on 4 bits or more was");
    }
    auto rate_at = [&](uint32_t t) {
        const double eff = cores > 1 ? 1.0 - (1.0 - pe) * (double) (t - 1) / (double) (cores - 1) : 1.0;
        return std::min(mem_plateau > 0.0 ? mem_plateau : 1e12, r1 * t * eff);
    };
    hw.n_cores = cores;
    hw.core_classes = {cores};
    hw.best_threads = cores;
    hw.thread_rates.clear();
    for (uint32_t t = 1; t < cores; t *= 2)
        hw.thread_rates.push_back({t, rate_at(t)});
    hw.thread_rates.push_back({cores, rate_at(cores)});
    const double plateau = rate_at(cores);
    hw.host_bandwidth_gibs = plateau;
    hw.host_expert_gibs = plateau * pm.host_expert_of_plateau.value;
    hw.host_dense_gibs = plateau * pm.host_dense_of_plateau.value;
    hw.host_wide_matmul_gibs = plateau * pm.host_wide_of_plateau.value;
    hw.gate_gemv_f32_us = hw.gate_gemv_int8_us = 0.0;
    notes.push_back("host plateau " + std::to_string((int) plateau) + " GiB/s: the lower of " +
                    std::to_string((int) mem_plateau) + " (memory, " + pct_range(pm.host_plateau_of_membw) + ") and " +
                    std::to_string((int) (r1 * cores * pe)) + " (cores); experts at " +
                    std::to_string((int) hw.host_expert_gibs) + " (" + pct_range(pm.host_expert_of_plateau) +
                    "), dense at " + std::to_string((int) hw.host_dense_gibs) + " (" +
                    pct_range(pm.host_dense_of_plateau) + ")");
    // ── storage: a measured curve's shape at the drive's sequential rate ──
    if (parts.storage_gbs > 0.0 && !pm.storage_shape.empty()) {
        const double peak_mibs = parts.storage_gbs * k_gb / (1024.0 * 1024.0);
        hw.storage.rate_curve = pm.storage_shape;
        for (RateSample & s : hw.storage.rate_curve)
            s.mib_per_s *= peak_mibs;
        const double scale = hw.storage.rate_unmapped_mibs > 0.0 ? 1.0 : 1.0;
        double peak = 0.0;
        for (const RateSample & s : pm.host_template.hw.storage.rate_curve)
            peak = std::max(peak, s.mib_per_s);
        if (peak > 0.0) {
            hw.storage.rate_unmapped_mibs = pm.host_template.hw.storage.rate_unmapped_mibs / peak * peak_mibs * scale;
            hw.storage.rate_mapped_mibs = pm.host_template.hw.storage.rate_mapped_mibs / peak * peak_mibs * scale;
        }
        notes.push_back("storage: a measured drive's curve by request size and lanes, at " +
                        std::to_string(parts.storage_gbs).substr(0, 4) + " GB/s at its peak");
    }
    // ── the cards ──
    const double xb = expert_layer_bytes(model), db = dense_layer_bytes(model);
    for (size_t i = 0; i < parts.cards.size(); ++i) {
        const GpuSpec & g = parts.cards[i].spec;
        const double link = parts.cards[i].link > 0.0 ? parts.cards[i].link : g.link;
        ComputeDevice d = pm.device_template;
        d.name = "SIM" + std::to_string(i);
        d.description = g.description.empty() ? g.id + " (simulated)" : g.description + " (simulated)";
        d.physical_id = "sim:" + std::to_string(i);
        const double free_share = pm.device_template.memory_total
                                      ? (double) pm.device_template.memory_free / pm.device_template.memory_total
                                      : 0.96;
        // A card reports a little less than its published size (the driver keeps some).
        const double reported = pm.gpu_reported_memory.value > 0.0 ? pm.gpu_reported_memory.value : 1.0;
        d.memory_total = (uint64_t) ((double) g.vram * reported);
        d.memory_free = (uint64_t) ((double) d.memory_total * free_share);
        const double bw = g.bw * k_gb;
        d.memory_bandwidth_gibs = pm.gpu_probe_bw.value * bw / k_gib;
        d.expert_gibs = layer_rate(xb, bw, pm.gpu_expert_eff, pm.gpu_expert_lat_s) / k_gib;
        d.dense_gibs = layer_rate(db, bw, pm.gpu_dense_eff, pm.gpu_dense_lat_s) / k_gib;
        d.wide_matmul_gibs = pm.gpu_wide_per_tflop.value * g.fp32;
        d.upload_host_buffer_gibs = pm.gpu_pinned.value * link * k_gb / k_gib;
        d.upload_gibs = pm.gpu_pageable.value * d.upload_host_buffer_gibs;
        if (pm.gpu_pageable_cap_gibs.value > 0.0)
            d.upload_gibs = std::min(d.upload_gibs, pm.gpu_pageable_cap_gibs.value);
        d.upload_gibs = std::min(d.upload_gibs, d.upload_host_buffer_gibs);
        d.split_seconds = pm.gpu_split_s.value;
        d.host_hop_seconds = pm.gpu_host_hop_s.value;
        hw.devices.push_back(d);
    }
    if (!parts.cards.empty()) {
        notes.push_back("cards: experts at " + std::to_string((int) hw.devices[0].expert_gibs) + " GiB/s (" +
                        pct_range(pm.gpu_expert_eff) + "), dense at " + std::to_string((int) hw.devices[0].dense_gibs) +
                        " (" + pct_range(pm.gpu_dense_eff) + "), uploads " +
                        std::to_string(hw.devices[0].upload_host_buffer_gibs).substr(0, 4) + " / " +
                        std::to_string(hw.devices[0].upload_gibs).substr(0, 4) + " GiB/s pinned / pageable (" +
                        pct_range(pm.gpu_pinned) + " / " + pct_range(pm.gpu_pageable) +
                        "); every card simulated as the measured cards were: CUDA");
    }
    // The CPU as a device, as the probes list it.
    ComputeDevice cpu;
    for (const ComputeDevice & d : pm.host_template.hw.devices)
        if (d.is_cpu) cpu = d;
    cpu.description = c.description.empty() ? c.id : c.description;
    cpu.memory_total = hw.memory_total;
    cpu.memory_free = hw.residency_budget;
    cpu.memory_bandwidth_gibs = plateau;
    hw.devices.push_back(cpu);
    hw.backends_placeable = (uint32_t) parts.cards.size();
    char label[160];
    std::snprintf(label, sizeof(label),
                  "%u cores, %llu MiB available of %llu MiB, %.0f GiB/s on this model's matmul "
                  "(simulated from parts)",
                  cores, (unsigned long long) (hw.residency_budget >> 20), (unsigned long long) (hw.memory_total >> 20),
                  plateau);
    hw.label = label;
    return hw;
}

std::string check_parts_model(const PartsCatalog & cat) {
    std::ostringstream o;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%-40s %-10s %10s %10s %10s %10s %10s %10s %10s %10s\n",
                  "left out (measured / modelled)", "quant", "host thr1", "plateau", "host exp", "card exp",
                  "card dense", "pinned", "pageable", "card mem");
    o << buf;
    std::vector<double> all;
    for (const Rig & rig : cat.rigs) {
        PartsModel pm;
        std::string err, text;
        if (!fit_parts_model(cat, pm, err, rig.record)) continue;
        PlanRecord rec;
        const std::string path = (std::filesystem::u8path(cat.records_dir) / (rig.record + ".planrec")).u8string();
        if (!read_file(path, text) || !parse_plan_record(text, rec, err)) continue;
        const auto cpu = cat.cpus.find(rig.cpu);
        if (cpu == cat.cpus.end()) continue;
        MachineParts parts;
        parts.ram = rec.hw.memory_total;
        parts.available = rec.hw.residency_budget;
        parts.cpu = cpu->second;
        parts.cpu.cores = rec.hw.n_cores ? rec.hw.n_cores : parts.cpu.cores; // the quota the record ran under
        const auto gpu = cat.gpus.find(rig.gpu);
        if (gpu != cat.gpus.end()) parts.cards.push_back({gpu->second, rig.link});
        std::vector<std::string> notes;
        const HardwareProfile hw = build_hardware(parts, pm, rec.model, notes);
        auto ratio = [&](double meas, double model) {
            if (meas <= 0.0 || model <= 0.0) return std::string("-");
            all.push_back(meas / model);
            char b[32];
            std::snprintf(b, sizeof(b), "%.2f", meas / model);
            return std::string(b);
        };
        const ComputeDevice * dm = first_card(rec.hw);
        const ComputeDevice * ds = first_card(hw);
        const bool card = dm && ds && dm->has_own_memory() && gpu != cat.gpus.end();
        std::snprintf(buf, sizeof(buf), "%-40.40s %-10s %10s %10s %10s %10s %10s %10s %10s %10s\n", rig.record.c_str(),
                      quant_name(rec.model.expert_type_id).c_str(), ratio(one_thread(rec.hw), one_thread(hw)).c_str(),
                      ratio(rec.hw.host_bandwidth_gibs, hw.host_bandwidth_gibs).c_str(),
                      ratio(rec.hw.host_expert_gibs, hw.host_expert_gibs).c_str(),
                      card ? ratio(dm->expert_gibs, ds->expert_gibs).c_str() : "-",
                      card ? ratio(dm->dense_gibs, ds->dense_gibs).c_str() : "-",
                      card ? ratio(dm->upload_host_buffer_gibs, ds->upload_host_buffer_gibs).c_str() : "-",
                      card ? ratio(dm->upload_gibs, ds->upload_gibs).c_str() : "-",
                      card ? ratio((double) dm->memory_total, (double) ds->memory_total).c_str() : "-");
        o << buf;
    }
    if (!all.empty()) {
        std::sort(all.begin(), all.end());
        std::snprintf(buf, sizeof(buf), "%zu rates: measured / modelled from %.2f to %.2f, median %.2f\n", all.size(),
                      all.front(), all.back(), all[all.size() / 2]);
        o << buf;
    }
    return o.str();
}

} // namespace bmoe
