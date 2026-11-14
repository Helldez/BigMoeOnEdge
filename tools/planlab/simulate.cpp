#include "simulate.h"

#include "sim_device.h"
#include "sim_parts.h"

#include "bmoe/auto_plan.h"
#include "bmoe/probe.h"
#include "bmoe/routing.h"
#include "bmoe/version.h"

#include "llama.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace bmoe {

namespace {

bool read_file(const std::string & path, std::string & text) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream all;
    all << f.rdbuf();
    text = all.str();
    return true;
}

// Adds a `# layer=N expert_bytes=B` line, B the median of the given ones, for every layer the trace
// routes but does not size. Returns how many it added.
int fill_unsized_layers(std::string & text) {
    std::istringstream in(text);
    std::string line, head, body;
    std::map<uint32_t, uint64_t> sized;
    std::set<uint32_t> routed;
    bool in_head = true;
    while (std::getline(in, line)) {
        if (in_head && (line.empty() || line[0] == '#')) {
            unsigned layer = 0;
            unsigned long long bytes = 0;
            if (std::sscanf(line.c_str(), "# layer=%u expert_bytes=%llu", &layer, &bytes) == 2 && bytes > 0)
                sized[layer] = bytes;
            head += line + "\n";
            continue;
        }
        in_head = false;
        unsigned layer = 0;
        char phase = 0;
        if (std::sscanf(line.c_str(), "%c %u", &phase, &layer) == 2) routed.insert(layer);
        body += line + "\n";
    }
    if (sized.empty()) return 0;
    std::vector<uint64_t> sizes;
    for (const auto & [l, b] : sized)
        sizes.push_back(b);
    std::sort(sizes.begin(), sizes.end());
    const uint64_t median = sizes[sizes.size() / 2];
    int added = 0;
    for (uint32_t l : routed)
        if (!sized.count(l)) {
            head += "# layer=" + std::to_string(l) + " expert_bytes=" + std::to_string(median) + "\n";
            ++added;
        }
    if (added) text = head + body;
    return added;
}

bool load_record(const std::string & path, PlanRecord & rec, std::string & error) {
    std::string text;
    if (!read_file(path, text)) {
        error = "cannot read " + path;
        return false;
    }
    if (!parse_plan_record(text, rec, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

// "16G", "16384M", "17179869184": bytes.
bool parse_size(const std::string & s, uint64_t & out) {
    if (s.empty()) return false;
    char * end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || v < 0.0) return false;
    const std::string unit = end;
    double mul = 1.0;
    if (unit == "G" || unit == "GiB" || unit == "g")
        mul = 1024.0 * 1024.0 * 1024.0;
    else if (unit == "M" || unit == "MiB" || unit == "m")
        mul = 1024.0 * 1024.0;
    else if (!unit.empty())
        return false;
    out = (uint64_t) std::llround(v * mul);
    return true;
}

std::string resolve(const std::string & base_dir, const std::string & p) {
    if (p.empty() || base_dir.empty()) return p;
    const std::filesystem::path path = std::filesystem::u8path(p);
    if (path.is_absolute()) return p;
    return (std::filesystem::u8path(base_dir) / path).lexically_normal().u8string();
}

std::string gib(uint64_t b) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f GiB", (double) b / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

} // namespace

bool parse_machine_spec(const std::string & text,
                        const std::string & base_dir,
                        MachineSpec & out,
                        std::string & error) {
    std::istringstream lines(text);
    std::string line;
    int n = 0;
    std::vector<std::pair<int, std::string>> parts_lines;
    while (std::getline(lines, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        line = line.substr(start);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = "line " + std::to_string(n) + ": not key=value";
            return false;
        }
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "note") {
            out.note = value;
        } else if (key == "host") {
            out.host = resolve(base_dir, value);
        } else if (key == "routing") {
            out.routing = resolve(base_dir, value);
        } else if (key == "hitcurve") {
            std::istringstream pts(value);
            std::string pt;
            while (std::getline(pts, pt, ',')) {
                const size_t colon = pt.find(':');
                uint64_t bytes = 0;
                if (colon == std::string::npos || !parse_size(pt.substr(0, colon), bytes)) {
                    error = "line " + std::to_string(n) + ": hitcurve wants SIZE:SHARE,...";
                    return false;
                }
                out.hit_points.emplace_back(bytes, std::atof(pt.c_str() + colon + 1));
            }
            std::sort(out.hit_points.begin(), out.hit_points.end());
        } else if (key == "set") {
            out.sets.push_back(value);
        } else if (key == "parts") {
            out.parts_catalog = resolve(base_dir, value);
            std::string text;
            if (!read_file(out.parts_catalog, text) ||
                !parse_parts_catalog(text, std::filesystem::u8path(out.parts_catalog).parent_path().u8string(),
                                     out.catalog, error)) {
                error =
                    "line " + std::to_string(n) + ": " + (error.empty() ? "cannot read " + out.parts_catalog : error);
                return false;
            }
        } else if (key == "ram" || key == "available" || key == "storage" || key == "cpu" || key == "gpu") {
            parts_lines.emplace_back(n, key + "=" + value);
        } else if (key == "device") {
            // device=RECORD[#INDEX] [memory=16G] [bandwidth=X] [compute=X] [link=X] [description=...]
            std::istringstream words(value);
            std::string w;
            SimDeviceFrom d;
            words >> w;
            const size_t hash = w.rfind('#');
            if (hash != std::string::npos) {
                d.index = std::atoi(w.c_str() + hash + 1);
                w = w.substr(0, hash);
            }
            d.record = resolve(base_dir, w);
            while (words >> w) {
                const size_t e = w.find('=');
                const std::string k = e == std::string::npos ? w : w.substr(0, e);
                const std::string v = e == std::string::npos ? "" : w.substr(e + 1);
                bool ok = true;
                if (k == "memory")
                    ok = parse_size(v, d.memory_total);
                else if (k == "bandwidth")
                    d.bandwidth_scale = std::atof(v.c_str());
                else if (k == "compute")
                    d.compute_scale = std::atof(v.c_str());
                else if (k == "link")
                    d.link_scale = std::atof(v.c_str());
                else if (k == "description") {
                    // the rest of the line: a description has spaces
                    std::string rest;
                    std::getline(words, rest);
                    d.description = v + rest;
                } else
                    ok = false;
                if (!ok) {
                    error = "line " + std::to_string(n) + ": device option '" + w + "' not understood";
                    return false;
                }
            }
            out.devices.push_back(d);
        } else {
            error = "line " + std::to_string(n) + ": unknown key '" + key + "'";
            return false;
        }
    }
    // The parts lines, once the catalog they name parts from is known.
    for (const auto & [ln, kv] : parts_lines) {
        if (out.parts_catalog.empty()) {
            error = "line " + std::to_string(ln) + ": a machine described by its parts needs parts=<catalog>";
            return false;
        }
        const size_t eq = kv.find('=');
        const std::string key = kv.substr(0, eq), value = kv.substr(eq + 1);
        out.parts_text += (out.parts_text.empty() ? "" : "; ") + kv;
        std::istringstream words(value);
        std::string id, w;
        words >> id;
        std::map<std::string, std::string> opt;
        while (words >> w) {
            const size_t e = w.find('=');
            if (e == std::string::npos) {
                error = "line " + std::to_string(ln) + ": '" + w + "' is not key=value";
                return false;
            }
            opt[w.substr(0, e)] = w.substr(e + 1);
        }
        auto size_opt = [&](const char * k, uint64_t & dst) {
            const auto it = opt.find(k);
            if (it != opt.end() && !parse_size(it->second, dst)) return false;
            return true;
        };
        auto num_opt = [&](const char * k, double & dst) {
            const auto it = opt.find(k);
            if (it != opt.end()) dst = std::atof(it->second.c_str());
        };
        if (key == "ram" || key == "available") {
            uint64_t b = 0;
            if (!parse_size(id, b)) {
                error = "line " + std::to_string(ln) + ": " + key + " wants a size (32G)";
                return false;
            }
            (key == "ram" ? out.parts.ram : out.parts.available) = b;
        } else if (key == "storage") {
            out.parts.storage_gbs = std::atof(id.c_str()); // GB/s, sequential read
        } else if (key == "cpu") {
            // cpu=ID [cores=N] [ghz=X] [membw=GB/s] [core=CLASS] [simd=...], or cpu=custom with all of them
            const auto it = out.catalog.cpus.find(id);
            if (it == out.catalog.cpus.end() && id != "custom") {
                error = "line " + std::to_string(ln) + ": no cpu '" + id + "' in " + out.parts_catalog;
                return false;
            }
            CpuSpec c = it != out.catalog.cpus.end() ? it->second : CpuSpec{};
            double cores = c.cores;
            num_opt("cores", cores);
            c.cores = (uint32_t) cores;
            num_opt("ghz", c.ghz);
            num_opt("membw", c.membw);
            num_opt("thread", c.thread_gibs);
            if (opt.count("core")) c.core = opt["core"];
            if (opt.count("simd")) c.simd = opt["simd"];
            if (id == "custom") c.id = "custom";
            out.parts.cpu = c;
        } else if (key == "gpu") {
            // gpu=ID [vram=16G] [bw=GB/s] [fp32=TF] [link=pcie4x16] [slot=pcie3x16], or gpu=custom with all of them
            const auto it = out.catalog.gpus.find(id);
            if (it == out.catalog.gpus.end() && id != "custom") {
                error = "line " + std::to_string(ln) + ": no gpu '" + id + "' in " + out.parts_catalog;
                return false;
            }
            MachineParts::Card card;
            card.spec = it != out.catalog.gpus.end() ? it->second : GpuSpec{};
            if (!size_opt("vram", card.spec.vram)) {
                error = "line " + std::to_string(ln) + ": vram wants a size";
                return false;
            }
            num_opt("bw", card.spec.bw);
            num_opt("fp32", card.spec.fp32);
            if (opt.count("link")) card.spec.link = link_gbs(opt["link"]);
            // The card gets the slower of its own link and the slot's.
            card.link = card.spec.link;
            if (opt.count("slot")) card.link = std::min(card.link, link_gbs(opt["slot"]));
            if (id == "custom") card.spec.id = "custom";
            out.parts.cards.push_back(card);
        }
    }
    if (out.host.empty() && out.parts_catalog.empty()) {
        error = "a machine needs host=<record>, or parts=<catalog> with ram=, cpu=, storage= and gpu= lines";
        return false;
    }
    if (!out.parts_catalog.empty() && (out.parts.ram == 0 || out.parts.cpu.cores == 0 || out.parts.cpu.membw <= 0.0)) {
        error = "a machine described by its parts needs ram= and a cpu= with cores and memory bandwidth";
        return false;
    }
    return true;
}

Simulation simulate_machine(const MachineSpec & spec,
                            const std::string & model_path,
                            const RunConfig & cfg_in,
                            const PlanRequest & req) {
    Simulation sim;
    auto caveat = [&sim](const std::string & c) {
        for (const std::string & have : sim.caveats)
            if (have == c) return;
        sim.caveats.push_back(c);
    };
    const std::string model_name = std::filesystem::u8path(model_path).filename().u8string();

    // ── the model: the real file's shapes, the routing of a record of it when given ──
    register_backends();
    ModelProfile model = probe_model(model_path.c_str());
    if (!model.ok) {
        sim.error = model_path + ": the model's header does not parse";
        return sim;
    }
    if (!spec.routing.empty() && std::filesystem::u8path(spec.routing).extension() == ".routing") {
        // A routing trace as bmoe-cli keeps it (`<fingerprint>.routing`): attached the way the engine
        // attaches it, so a trace of another model is rejected with its reason.
        std::string text, err;
        RoutingTrace trace;
        if (read_file(spec.routing, text)) {
            // 0.43 keeps a streamed run's routing of every MoE layer but the expert bytes of the streamed
            // ones only, so a layer whose experts stayed on a card has routing and no size, and the trace
            // does not parse. Such a layer is given the size of the others, said as a caveat.
            const int filled = fill_unsized_layers(text);
            if (filled > 0)
                caveat("the routing trace gave no expert size for " + std::to_string(filled) +
                       " layer(s) it routed (their experts were on a card in that run): given the median size "
                       "of the others");
        }
        if (text.empty() || !parse_routing_trace(text, trace, err)) {
            sim.error = spec.routing + ": " + (err.empty() ? "unreadable" : err);
            return sim;
        }
        attach_routing(model, trace, cfg_in.n_expert_used > 0 ? (uint32_t) cfg_in.n_expert_used : model.n_expert_used,
                       cfg_in.moe.cache_freq_half_life);
        if (!model.hit_curve.rejected.empty())
            caveat("the routing trace " + spec.routing + " was rejected: " + model.hit_curve.rejected);
    } else if (!spec.routing.empty()) {
        PlanRecord r;
        if (!load_record(spec.routing, r, sim.error)) return sim;
        if (r.model.file_bytes != model.file_bytes || r.model.arch != model.arch)
            caveat("the hit curve is " + spec.routing + "'s, a file of another size or architecture");
        model.hit_curve = r.model.hit_curve;
    } else if (!spec.hit_points.empty()) {
        HitCurve curve;
        curve.measured = true;
        curve.points.push_back({0, 0.0});
        double last = 0.0;
        for (const auto & [bytes, share] : spec.hit_points) {
            last = std::max(last, std::clamp(share, 0.0, 1.0)); // monotone, as a curve must be
            curve.points.push_back({bytes, last});
        }
        model.hit_curve = curve;
        caveat("the hit curve is made of measured runs' cache hit rates (hitcurve=), not a replay of this "
               "model's routing: a cache of another policy or another conversation serves another share");
    } else if (model.is_moe) {
        caveat("no routing of this model was given (routing=): the cache's hit rate is the planner's "
               "conservative default, as on a first load");
    }

    HardwareProfile hw;
    PlanRecord host;
    std::vector<SimDeviceSpec> sims;
    if (!spec.parts_catalog.empty()) {
        // ── any machine from its parts: every rate from the model of parts the records fit ──
        PartsModel pm;
        if (!fit_parts_model(spec.catalog, pm, sim.error)) return sim;
        std::vector<std::string> notes;
        hw = build_hardware(spec.parts, pm, model, notes);
        for (const std::string & n : notes)
            caveat(n);
        caveat("the model of parts is fitted on " + std::to_string(pm.fitted_on.size()) + " measured machines (" +
               spec.parts_catalog +
               "): a rate is a fit, and the notes above say how far "
               "those machines spread around it");
        for (const ComputeDevice & d : hw.devices)
            if (!d.is_cpu) sims.push_back({d.description, d.memory_total, d.memory_free});
        host = pm.host_template;
    } else {
        // ── the host: one machine's measured facts, without its cards ──
        if (!load_record(spec.host, host, sim.error)) return sim;
        hw = host.hw;
        std::vector<ComputeDevice> cpus;
        for (const ComputeDevice & d : hw.devices)
            if (d.is_cpu) cpus.push_back(d);
        hw.devices.clear();
        // ── the cards: each one a measured card, sized and scaled ──
        for (size_t i = 0; i < spec.devices.size(); ++i) {
            const SimDeviceFrom & from = spec.devices[i];
            PlanRecord rec;
            if (!load_record(from.record, rec, sim.error)) return sim;
            const ComputeDevice * src = nullptr;
            int k = 0;
            for (const ComputeDevice & d : rec.hw.devices) {
                if (from.index >= 0 ? k == from.index : !d.is_cpu) {
                    src = &d;
                    break;
                }
                ++k;
            }
            if (!src || src->is_cpu) {
                sim.error = from.record + ": no card at that index";
                return sim;
            }
            ComputeDevice d = *src;
            const std::string measured = d.description;
            d.name = "SIM" + std::to_string(i);
            d.physical_id = "sim:" + std::to_string(i);
            if (from.memory_total) {
                // What the driver kept of the measured card, kept in proportion.
                const double free_share =
                    d.memory_total ? (double) d.memory_free / (double) d.memory_total : 1.0 - 0.0366;
                d.memory_total = from.memory_total;
                d.memory_free = (uint64_t) ((double) from.memory_total * free_share);
            }
            d.memory_bandwidth_gibs *= from.bandwidth_scale;
            d.expert_gibs *= from.bandwidth_scale;
            d.dense_gibs *= from.bandwidth_scale;
            d.wide_matmul_gibs *= from.compute_scale;
            d.upload_gibs *= from.link_scale;
            d.upload_host_buffer_gibs *= from.link_scale;
            d.description =
                !from.description.empty() ? from.description : measured + " (simulated, " + gib(d.memory_total) + ")";
            if (from.bandwidth_scale != 1.0 || from.compute_scale != 1.0 || from.link_scale != 1.0)
                caveat(d.name + " is " + measured + " as measured, its rates scaled (bandwidth x" +
                       std::to_string(from.bandwidth_scale).substr(0, 5) + ", compute x" +
                       std::to_string(from.compute_scale).substr(0, 5) + ", link x" +
                       std::to_string(from.link_scale).substr(0, 5) +
                       "): a ratio of specifications, not a measurement");
            if (rec.env.count("model_file") && rec.env.at("model_file") != model_name)
                caveat(d.name + "'s measured rates and op support were read on " + rec.env.at("model_file") +
                       ": a card's rate on another quantization is not the same rate");
            hw.devices.push_back(d);
            sims.push_back({d.description, d.memory_total, d.memory_free});
        }
        for (const ComputeDevice & c : cpus)
            hw.devices.push_back(c);
        if (!spec.devices.empty()) hw.backends_placeable = (int) spec.devices.size();
        if (host.env.count("model_file") && host.env.at("model_file") != model_name)
            caveat("the host's rates were measured on " + host.env.at("model_file") +
                   ": a host's matmul rate depends on the quantization it computes");
    }

    // A fact changed is its line again, as in a replay: the record's own text, the sets appended.
    if (!spec.sets.empty()) {
        PlanRecord tmp;
        tmp.hw = hw;
        std::string text = format_plan_record(tmp);
        for (const std::string & kv : spec.sets)
            text += kv + "\n";
        std::string err;
        if (!parse_plan_record(text, tmp, err)) {
            sim.error = "a set= line: " + err;
            return sim;
        }
        hw = tmp.hw;
    }
    const std::vector<std::string> names = register_sim_devices(sims);

    // ── the plan: the real fitter on the simulated cards, the real rules on everything ──
    llama_backend_init();
    // Asked for, every line llama.cpp writes while it fits, the debug ones too: the fitter's own
    // breakdown per buffer type is what a simulated placement is compared with a real load's by.
    // Every line is also read for the one thing a simulated fit is known to get wrong: a fused op the
    // graph resolution turned off because its node and its layer landed on different devices. Measured
    // on DeepSeek V4.1 rebuilt from a 12 GB card's record: the simulated fit turned one off at the
    // boundary between the card's layers and the host's, the real one did not, and the host compute it
    // projected grew by gigabytes, so the card took fewer layers. Where it happens it is said.
    static bool debug = false;
    static int fused_off = 0;
    debug = cfg_in.llama_debug_log;
    fused_off = 0;
    llama_log_set(
        [](ggml_log_level level, const char * text, void *) {
            if (std::strstr(text, "not supported, set to disabled")) ++fused_off;
            if (debug || level >= GGML_LOG_LEVEL_ERROR) std::fputs(text, stderr);
        },
        nullptr);
    RunConfig cfg = cfg_in;
    cfg.model_path = model_path;
    AutoPlan facts;
    facts.hw = hw;
    facts.model = model;
    facts.backends = names;
    facts.placement = probe_placement(model_path.c_str(), model, hw, (uint32_t) cfg.n_ctx, (uint32_t) cfg.graph_width(),
                                      cfg.cache_type_k, cfg.cache_type_v, names, &cfg.spec);
    facts.refit = [&](const AutoPlan::FitKey & key) {
        RunConfig at = cfg;
        at.n_ctx = (int) key.n_ctx;
        return probe_placement(model_path.c_str(), model, hw, key.n_ctx, key.width, key.type_k, key.type_v, names,
                               &at.spec);
    };
    sim.plan = replan(facts, cfg, req, model.knobs);
    facts.refit = nullptr;
    if (fused_off > 0)
        caveat("the simulated fit turned a fused op off " + std::to_string(fused_off) +
               " time(s) (its node and its layer on different devices): a real card's fit may keep it, and then "
               "projects less host compute and places more layers than this (docs/hardware-simulation.md)");

    sim.record = make_plan_record(cfg, req, hw, model, placements_of(facts, cfg), sim.plan);
    sim.record.note = spec.note.empty() ? "simulated machine" : spec.note + " (simulated)";
    sim.record.env = host.env;
    sim.record.env["simulated"] = "true";
    if (!spec.host.empty())
        sim.record.env["simulated_host"] = std::filesystem::u8path(spec.host).filename().u8string();
    else
        for (const std::string & k : {"cpu", "kernel", "os", "os_version", "arch", "swap_kind", "swap_total_bytes",
                                      "cgroup_memory_max", "swappiness", "transparent_hugepage"})
            sim.record.env.erase(k);
    if (!spec.parts_catalog.empty()) sim.record.env["simulated_from_parts"] = spec.parts_text;
    for (size_t i = 0; i < spec.devices.size(); ++i)
        sim.record.env["simulated_device_" + std::to_string(i)] =
            std::filesystem::u8path(spec.devices[i].record).filename().u8string();
    sim.record.env["model_file"] = std::filesystem::u8path(model_path).filename().u8string();
    // The model is the one simulated, not the one the host's facts were recorded on.
    sim.record.env["model_fingerprint"] = model_fingerprint(model_path.c_str());
    return sim;
}

} // namespace bmoe
