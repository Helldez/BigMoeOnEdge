// A plan's inputs as text (bmoe/plan_record.h), and the plan made again from them.
//
// Every field of the three profiles is named ONCE, in the `fields` functions below, which both the
// writer and the reader walk: there is no second list to keep in step. A field added to a profile
// and not named here is not in a record, and a replay then plans on its default - which the capture
// catches when it matters, because the front-end replays what it just recorded and compares.

#include "plan_record.h"

#include "bmoe/measured_knobs.h"
#include "bmoe/params.h"
#include "bmoe/planner.h"
#include "bmoe/version.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace bmoe {

namespace {

constexpr const char * k_header = "bmoe-plan-record 1";
// A list longer than this is a damaged record, not a machine.
constexpr uint32_t k_max_list = 1u << 20;

// ── values as text ──────────────────────────────────────────────────────────────────────────
std::string text(uint64_t v) {
    return std::to_string((unsigned long long) v);
}
std::string text(uint32_t v) {
    return std::to_string(v);
}
std::string text(int v) {
    return std::to_string(v);
}
std::string text(bool v) {
    return v ? "true" : "false";
}
// Enough digits to read back the same value: a record is replayed, and a rate off by one ulp is a
// threshold away from another plan.
std::string text(double v) {
    char b[40];
    std::snprintf(b, sizeof(b), "%.17g", v);
    return b;
}
std::string text(float v) {
    char b[40];
    std::snprintf(b, sizeof(b), "%.9g", (double) v);
    return b;
}
// One line per value, so a line break in a string is spelled.
std::string text(const std::string & v) {
    std::string out;
    for (char c : v) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else
            out += c;
    }
    return out;
}
std::string text(Tri v) {
    return v == Tri::Yes ? "yes" : v == Tri::No ? "no" : "unknown";
}

constexpr const char * k_overflow[] = {"unknown", "none", "compress", "swap", "refault", "kill"};
constexpr const char * k_headroom[] = {"unknown", "compressible", "measured"};
std::string text(Overflow v) {
    return k_overflow[(int) v];
}
std::string text(Headroom v) {
    return k_headroom[(int) v];
}

template <class T> std::string text(const std::vector<T> & v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i)
        out += (i ? "," : "") + text(v[i]);
    return out;
}

bool read(const std::string & s, uint64_t & v) {
    char * end = nullptr;
    if (s.empty() || s[0] == '-') return false;
    v = (uint64_t) std::strtoull(s.c_str(), &end, 10);
    return !*end;
}
bool read(const std::string & s, uint32_t & v) {
    uint64_t wide = 0;
    if (!read(s, wide) || wide > 0xffffffffull) return false;
    v = (uint32_t) wide;
    return true;
}
bool read(const std::string & s, int & v) {
    char * end = nullptr;
    if (s.empty()) return false;
    v = (int) std::strtol(s.c_str(), &end, 10);
    return !*end;
}
bool read(const std::string & s, bool & v) {
    if (s != "true" && s != "false") return false;
    v = s == "true";
    return true;
}
bool read(const std::string & s, double & v) {
    char * end = nullptr;
    if (s.empty()) return false;
    v = std::strtod(s.c_str(), &end);
    return !*end;
}
bool read(const std::string & s, float & v) {
    double wide = 0;
    if (!read(s, wide)) return false;
    v = (float) wide;
    return true;
}
bool read(const std::string & s, std::string & v) {
    v.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            v += s[i];
            continue;
        }
        const char c = s[++i];
        v += c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    }
    return true;
}
bool read(const std::string & s, Tri & v) {
    if (s == "yes")
        v = Tri::Yes;
    else if (s == "no")
        v = Tri::No;
    else if (s == "unknown")
        v = Tri::Unknown;
    else
        return false;
    return true;
}
template <class E, size_t N> bool read_named(const std::string & s, const char * const (&names)[N], E & v) {
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) {
            v = (E) i;
            return true;
        }
    return false;
}
bool read(const std::string & s, Overflow & v) {
    return read_named(s, k_overflow, v);
}
bool read(const std::string & s, Headroom & v) {
    return read_named(s, k_headroom, v);
}
template <class T> bool read(const std::string & s, std::vector<T> & v) {
    v.clear();
    if (s.empty()) return true;
    std::stringstream parts(s);
    std::string part;
    while (std::getline(parts, part, ',')) {
        T one{};
        if (!read(part, one)) return false;
        v.push_back(one);
    }
    return true;
}

// ── the one walk both directions share ──────────────────────────────────────────────────────
struct Io {
    bool writing = true;
    std::string out;
    const std::map<std::string, std::string> * in = nullptr;
    std::set<std::string> * used = nullptr;
    std::string * error = nullptr;

    template <class T> void f(const std::string & key, T & v) {
        if (writing) {
            out += key + "=" + text(v) + "\n";
            return;
        }
        const auto it = in->find(key);
        if (it == in->end()) return;
        used->insert(key);
        if (!read(it->second, v) && error->empty()) *error = "'" + key + "': cannot read '" + it->second + "'";
    }

    // A list of records: how many under `key`, then each under `key.<i>`.
    template <class T, class Each> void list(const std::string & key, std::vector<T> & v, Each each) {
        uint32_t n = (uint32_t) v.size();
        f(key, n);
        if (!writing) {
            if (n > k_max_list) {
                if (error->empty()) *error = "'" + key + "': " + std::to_string(n) + " entries is not a record";
                return;
            }
            v.resize(n);
        }
        for (uint32_t i = 0; i < n; ++i)
            each(key + "." + std::to_string(i), v[i]);
    }

    void strings(const std::string & key, std::vector<std::string> & v) {
        list(key, v, [this](const std::string & k, std::string & s) { f(k, s); });
    }
    template <class T> void named(const std::string & key, std::vector<std::pair<std::string, T>> & v) {
        list(key, v, [this](const std::string & k, std::pair<std::string, T> & p) {
            f(k + ".name", p.first);
            f(k + ".value", p.second);
        });
    }
};

void fields(Io & io, const std::string & p, ComputeDevice & d) {
    io.f(p + ".name", d.name);
    io.f(p + ".description", d.description);
    io.f(p + ".physical_id", d.physical_id);
    io.f(p + ".memory_free", d.memory_free);
    io.f(p + ".memory_total", d.memory_total);
    io.f(p + ".is_cpu", d.is_cpu);
    io.f(p + ".usable", d.usable);
    io.f(p + ".unusable_reason", d.unusable_reason);
    io.f(p + ".probed_out_of_process", d.probed_out_of_process);
    io.f(p + ".shares_host_memory", d.shares_host_memory);
    io.f(p + ".buffers_locked", d.buffers_locked);
    io.f(p + ".host_buffer", d.host_buffer);
    io.f(p + ".host_ptr_buffers", d.host_ptr_buffers);
    io.f(p + ".async_copies", d.async_copies);
    io.f(p + ".rebindable", d.rebindable);
    io.f(p + ".runs_expert_op", d.runs_expert_op);
    io.f(p + ".needs_repack", d.needs_repack);
    io.f(p + ".memory_bandwidth_gibs", d.memory_bandwidth_gibs);
    io.f(p + ".expert_gibs", d.expert_gibs);
    io.f(p + ".dense_gibs", d.dense_gibs);
    io.f(p + ".wide_matmul_gibs", d.wide_matmul_gibs);
    io.f(p + ".wide_identity_ok", d.wide_identity_ok);
    io.f(p + ".identity_ok", d.identity_ok);
    io.f(p + ".host_ptr_verified", d.host_ptr_verified);
    io.f(p + ".split_seconds", d.split_seconds);
    io.f(p + ".graph_splits", d.graph_splits);
    io.f(p + ".upload_gibs", d.upload_gibs);
    io.f(p + ".upload_host_buffer_gibs", d.upload_host_buffer_gibs);
}

void fields(Io & io, const std::string & p, StorageFacts & s) {
    io.f(p + ".align", s.align);
    io.f(p + ".direct_ok", s.direct_ok);
    io.f(p + ".mapping_serialises_reads", s.mapping_serialises_reads);
    io.list(p + ".rate_curve", s.rate_curve, [&io](const std::string & k, RateSample & r) {
        io.f(k + ".request_bytes", r.request_bytes);
        io.f(k + ".lanes", r.lanes);
        io.f(k + ".mib_per_s", r.mib_per_s);
    });
    io.f(p + ".rate_mapped_mibs", s.rate_mapped_mibs);
    io.f(p + ".rate_unmapped_mibs", s.rate_unmapped_mibs);
    io.f(p + ".refault_mibs", s.refault_mibs);
    io.f(p + ".rate_spread", s.rate_spread);
}

void fields(Io & io, const std::string & p, HardwareProfile & h) {
    io.f(p + ".label", h.label);
    io.f(p + ".residency_budget", h.residency_budget);
    io.f(p + ".memory_total", h.memory_total);
    io.f(p + ".anon_overflow", h.anon_overflow);
    io.f(p + ".holdable_bytes", h.holdable_bytes);
    io.f(p + ".holdable_from", h.holdable_from);
    io.f(p + ".reclaim_exempt_max", h.reclaim_exempt_max);
    io.f(p + ".lockable_bytes", h.lockable_bytes);
    io.f(p + ".lockable_bytes_known", h.lockable_bytes_known);
    io.f(p + ".lock_in_place_bytes", h.lock_in_place_bytes);
    io.f(p + ".lock_in_place_bytes_known", h.lock_in_place_bytes_known);
    io.f(p + ".discardable_memory", h.discardable_memory);
    io.f(p + ".n_cores", h.n_cores);
    io.f(p + ".core_classes", h.core_classes);
    io.f(p + ".best_threads", h.best_threads);
    io.list(p + ".thread_rates", h.thread_rates, [&io](const std::string & k, HardwareProfile::ThreadRate & r) {
        io.f(k + ".threads", r.threads);
        io.f(k + ".gibs", r.gibs);
    });
    io.f(p + ".host_bandwidth_gibs", h.host_bandwidth_gibs);
    io.f(p + ".host_expert_gibs", h.host_expert_gibs);
    io.f(p + ".host_dense_gibs", h.host_dense_gibs);
    io.f(p + ".host_wide_matmul_gibs", h.host_wide_matmul_gibs);
    io.f(p + ".wide_batch", h.wide_batch);
    io.f(p + ".gate_gemv_f32_us", h.gate_gemv_f32_us);
    io.f(p + ".gate_gemv_int8_us", h.gate_gemv_int8_us);
    io.f(p + ".backends_linked", h.backends_linked);
    io.f(p + ".backends_loaded", h.backends_loaded);
    io.f(p + ".backends_looked", h.backends_looked);
    io.f(p + ".backends_placeable", h.backends_placeable);
    io.list(p + ".devices", h.devices, [&io](const std::string & k, ComputeDevice & d) { fields(io, k, d); });
    fields(io, p + ".storage", h.storage);
}

void fields(Io & io, const std::string & p, ModelProfile & m) {
    io.f(p + ".ok", m.ok);
    io.f(p + ".error", m.error);
    io.f(p + ".arch", m.arch);
    io.f(p + ".is_moe", m.is_moe);
    io.f(p + ".n_layer", m.n_layer);
    io.f(p + ".n_moe_layer", m.n_moe_layer);
    io.f(p + ".moe_layers", m.moe_layers);
    io.f(p + ".n_expert", m.n_expert);
    io.f(p + ".n_expert_used", m.n_expert_used);
    io.f(p + ".n_embd", m.n_embd);
    io.f(p + ".n_expert_projections", m.n_expert_projections);
    io.f(p + ".file_bytes", m.file_bytes);
    io.f(p + ".expert_bytes", m.expert_bytes);
    io.f(p + ".dense_bytes", m.dense_bytes);
    io.f(p + ".largest_dense_tensor", m.largest_dense_tensor);
    io.f(p + ".mtp_head_bytes", m.mtp_head_bytes);
    io.f(p + ".projector_bytes", m.projector_bytes);
    io.strings(p + ".embedding_tensors", m.embedding_tensors);
    io.f(p + ".expert_file", m.expert_file);
    io.f(p + ".largest_expert_layer_bytes", m.largest_expert_layer_bytes);
    io.f(p + ".largest_layer_dense_bytes", m.largest_layer_dense_bytes);
    for (int g = 0; g < (int) WeightGroup::count; ++g) {
        const std::string k = p + ".group." + group_name((WeightGroup) g);
        io.f(k + ".bytes", m.groups[g].bytes);
        io.f(k + ".bytes_per_token", m.groups[g].bytes_per_token);
        io.f(k + ".row_gatherable", m.groups[g].row_gatherable);
        io.f(k + ".streamable", m.groups[g].streamable);
    }
    io.f(p + ".expert_slice_bytes", m.expert_slice_bytes);
    io.f(p + ".token_cycle_bytes", m.token_cycle_bytes);
    io.f(p + ".expert_type_id", m.expert_type_id);
    io.f(p + ".expert_type_ids", m.expert_type_ids);
    io.f(p + ".tied_output_head", m.tied_output_head);
    io.f(p + ".has_mtp", m.has_mtp);
    io.f(p + ".hit_curve.measured", m.hit_curve.measured);
    io.f(p + ".hit_curve.decode_tokens", m.hit_curve.decode_tokens);
    io.f(p + ".hit_curve.prefill_tokens", m.hit_curve.prefill_tokens);
    io.f(p + ".hit_curve.rejected", m.hit_curve.rejected);
    io.list(p + ".hit_curve.points", m.hit_curve.points, [&io](const std::string & k, HitPoint & h) {
        io.f(k + ".cache_bytes", h.cache_bytes);
        io.f(k + ".hit", h.hit);
    });

    // What an earlier load measured is a record with a text of its own (bmoe/measured_knobs.h):
    // kept as that text's one line rather than spelled a second time here.
    std::string knobs;
    if (io.writing && !m.knobs.empty()) {
        const std::string all = format_measured_knobs({m.knobs});
        const size_t a = all.find('\n') + 1, b = all.find('\n', a);
        knobs = all.substr(a, b - a);
    }
    io.f(p + ".knobs", knobs);
    if (!io.writing && !knobs.empty()) {
        std::vector<MeasuredKnobs> parsed;
        std::string err;
        if (parse_measured_knobs(format_measured_knobs({}) + knobs + "\n", parsed, err) && parsed.size() == 1)
            m.knobs = parsed[0];
        else if (io.error->empty())
            *io.error = "'" + p + ".knobs': " + err;
    }
}

void fields(Io & io, const std::string & p, Placement & q) {
    io.f(p + ".fitted", q.fitted);
    io.f(p + ".outcome", q.outcome);
    io.f(p + ".n_gpu_layers", q.n_gpu_layers);
    io.f(p + ".n_ctx", q.n_ctx);
    io.f(p + ".host_expert_layers", q.host_expert_layers);
    io.f(p + ".host_resident_bytes", q.host_resident_bytes);
    io.f(p + ".device_bytes", q.device_bytes);
    io.f(p + ".device_projector_bytes", q.device_projector_bytes);
    io.f(p + ".shared_memory_placement", q.shared_memory_placement);
    io.f(p + ".raw_host_model_bytes", q.raw_host_model_bytes);
    io.f(p + ".raw_host_context_bytes", q.raw_host_context_bytes);
    io.f(p + ".raw_host_compute_bytes", q.raw_host_compute_bytes);
    io.f(p + ".device_compute_bytes", q.device_compute_bytes);
    io.f(p + ".device_context_bytes", q.device_context_bytes);
    io.f(p + ".context_fixed_bytes", q.context_fixed_bytes);
    io.strings(p + ".projected_devices", q.projected_devices);
    io.f(p + ".projected_compute_bytes", q.projected_compute_bytes);
    io.f(p + ".compute_width", q.compute_width);
    io.f(p + ".host_compute_per_token", q.host_compute_per_token);
    io.f(p + ".device_compute_per_token", q.device_compute_per_token);
    io.strings(p + ".override_patterns", q.override_patterns);
    io.strings(p + ".all_expert_patterns", q.all_expert_patterns);
    io.named(p + ".device_split", q.device_split);
    io.named(p + ".device_spare", q.device_spare);
}

void fields(Io & io, PlanRecord & r) {
    io.f("engine", r.engine);
    io.f("note", r.note);
    io.named("config", r.config);
    io.strings("request.pinned", r.request.pinned);
    io.f("request.quality_budget", r.request.quality_budget);
    io.f("request.measure_knobs", r.request.measure_knobs);
    fields(io, "hw", r.hw);
    fields(io, "model", r.model);
    io.list("placement", r.placements, [&io](const std::string & k, PlacedAt & p) {
        io.f(k + ".asked_ubatch", p.n_ubatch);
        fields(io, k, p.placement);
    });
    io.f("plan.resolved", r.resolved);
    io.f("plan.predicted_s_per_token", r.predicted_s_per_token);
    io.f("observed.generated_tokens", r.observed.generated_tokens);
    io.f("observed.seconds_per_token", r.observed.seconds_per_token);
    io.f("observed.compute_s_per_token", r.observed.compute_s_per_token);
    io.f("observed.io_s_per_token", r.observed.io_s_per_token);
    io.f("observed.cache_hit_decode_pct", r.observed.cache_hit_decode_pct);
    io.f("observed.prompt_tokens", r.observed.prompt_tokens);
    io.f("observed.prefill_seconds", r.observed.prefill_seconds);
}

// A path as the rules read it: whether there is one. Where it is on someone's drive is not a fact
// about the machine and has no place in a file that is committed.
std::string file_name(const std::string & path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

RunConfig without_locations(RunConfig cfg) {
    cfg.model_path = RunConfig().model_path;
    cfg.spec.mtp_head = file_name(cfg.spec.mtp_head);
    for (const ParamDesc & d : params()) {
        if (d.type != ParamType::Path) continue;
        const std::string v = d.get(cfg);
        std::string err;
        if (!v.empty() && file_name(v) != v) d.set(cfg, file_name(v), err);
    }
    return cfg;
}

} // namespace

std::string resolved_line(const Plan & plan) {
    std::string line = std::string("regime=") + regime_name(plan.regime);
    for (const std::string & arg : to_args(without_locations(plan.config)))
        line += " " + arg;
    return line;
}

PlanRecord make_plan_record(const RunConfig & base,
                            const PlanRequest & request,
                            const HardwareProfile & hw,
                            const ModelProfile & model,
                            const std::vector<PlacedAt> & placements,
                            const Plan & plan) {
    PlanRecord r;
    r.engine = version();
    const RunConfig cfg = without_locations(base), defaults;
    for (const ParamDesc & d : params()) {
        const std::string v = d.get(cfg);
        if (v != d.get(defaults)) r.config.emplace_back(d.key, v);
    }
    r.request = request;
    r.hw = hw;
    r.model = model;
    r.model.expert_file = file_name(model.expert_file);
    r.placements = placements;
    r.resolved = resolved_line(plan);
    r.predicted_s_per_token = plan.allocation.seconds_per_token;
    return r;
}

std::string format_plan_record(const PlanRecord & rec) {
    Io io;
    io.out = std::string(k_header) + "\n";
    PlanRecord copy = rec; // the walk takes references both ways
    fields(io, copy);
    return io.out;
}

bool parse_plan_record(const std::string & text,
                       PlanRecord & out,
                       std::string & error,
                       std::vector<std::string> * unknown) {
    out = PlanRecord{};
    error.clear();
    std::istringstream in(text);
    std::string line;
    auto chomp = [](std::string & s) {
        if (!s.empty() && s.back() == '\r') s.pop_back();
    };
    if (!std::getline(in, line) || (chomp(line), line != k_header)) {
        error = "not a plan record (no '" + std::string(k_header) + "' header)";
        return false;
    }
    std::map<std::string, std::string> kv;
    int n = 1;
    while (std::getline(in, line)) {
        ++n;
        chomp(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            error = "line " + std::to_string(n) + ": not key=value";
            return false;
        }
        kv[line.substr(0, eq)] = line.substr(eq + 1); // the last one wins
    }
    std::set<std::string> used;
    Io io;
    io.writing = false;
    io.in = &kv;
    io.used = &used;
    io.error = &error;
    fields(io, out);
    if (!error.empty()) return false;
    if (unknown) {
        unknown->clear();
        for (const auto & e : kv)
            if (!used.count(e.first)) unknown->push_back(e.first);
    }
    return true;
}

RunConfig record_config(const PlanRecord & rec, std::vector<std::string> & problems) {
    RunConfig cfg;
    for (const auto & kv : rec.config) {
        const ParamDesc * d = find_param(kv.first);
        std::string err;
        if (!d)
            problems.push_back("'" + kv.first + "' is not a parameter of this build");
        else if (!d->set(cfg, kv.second, err))
            problems.push_back(kv.first + "=" + kv.second + ": " + err);
    }
    return cfg;
}

Replay replay_plan(const PlanRecord & rec, const RunConfig & cfg, const PlanRequest & request) {
    Replay out;
    auto caveat = [&out](const std::string & c) {
        for (const std::string & have : out.caveats)
            if (have == c) return;
        out.caveats.push_back(c);
    };

    // The fitter placed for one context and one pair of KV types; a replay that changes them plans
    // on a placement made for something else, and has to say so.
    std::vector<std::string> ignored;
    const RunConfig was = record_config(rec, ignored);
    if (!rec.placements.empty() && rec.placements.front().placement.fitted) {
        const ParamDesc * shaped[] = {find_param("ctx-size"), find_param("cache-type-k"), find_param("cache-type-v"),
                                      find_param("device-list"), find_param("devices")};
        for (const ParamDesc * d : shaped)
            if (d && d->get(cfg) != d->get(was))
                caveat("the recorded placement was made with " + d->key + "=" + d->get(was) + ", not " + d->get(cfg) +
                       ": the capacity fitter was not asked again");
    }

    const PlacementSource place = [&](uint32_t n_ubatch) {
        for (const PlacedAt & p : rec.placements)
            if (p.n_ubatch == n_ubatch) return p.placement;
        if (rec.placements.empty()) return Placement{};
        // Not a width that machine's fitter was asked at. The compute buffers move along the slope it
        // measured; which layers fit does not move, so a narrower graph is never credited with a
        // layer the fitter did not place.
        const Placement & first = rec.placements.front().placement;
        const uint32_t width = n_ubatch > 0 ? n_ubatch : first.compute_width;
        caveat("no placement was recorded " + std::to_string(width) +
               " tokens wide: the first one was moved along its measured slope, layers unchanged");
        return first.at_width(width);
    };
    out.plan = plan_run_placed(cfg, rec.hw, rec.model, request, place);
    return out;
}

} // namespace bmoe
