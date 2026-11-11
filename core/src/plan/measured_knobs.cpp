#include "bmoe/measured_knobs.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <utility>
#include <vector>

namespace bmoe {

std::string hardware_fingerprint(const HardwareProfile & hw) {
    // The facts as one canonical string, then hashed: a reader of a stored record has no use for
    // the facts themselves, only for whether they are this machine's.
    std::string facts = "cores=" + std::to_string(hw.n_cores) + ";classes=";
    for (uint32_t c : hw.core_classes)
        facts += std::to_string(c) + ",";
    facts += ";memory=" + std::to_string((unsigned long long) hw.memory_total);
    for (const ComputeDevice & d : hw.devices)
        facts +=
            ";device=" + d.name + "/" + std::to_string((unsigned long long) d.memory_total) + (d.is_cpu ? "/cpu" : "");
    uint64_t h = 1469598103934665603ull; // FNV-1a, 64-bit
    for (unsigned char ch : facts) {
        h ^= ch;
        h *= 1099511628211ull;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long) h);
    return hex;
}

namespace {

constexpr const char * k_header = "bmoe-measured-knobs 1";

std::string ints(const std::vector<int> & v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += (i ? "," : "") + std::to_string(v[i]);
    return s;
}

bool parse_ints(const std::string & s, std::vector<int> & out) {
    out.clear();
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        char * end = nullptr;
        const long v = std::strtol(part.c_str(), &end, 10);
        if (part.empty() || *end) return false;
        out.push_back((int) v);
    }
    return true;
}

// The sides a record can mark as not viable, by the name the text format gives them.
std::vector<std::pair<const char *, bool>> unviable_sides(const MeasuredKnobs & r) {
    return {{"device_cache", r.device_cache_unviable},
            {"no_device_cache", r.no_device_cache_unviable},
            {"wide", r.wide_unviable},
            {"narrow", r.narrow_unviable}};
}

bool parse_unviable(const std::string & s, MeasuredKnobs & r) {
    if (s.empty()) return false;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (part == "device_cache")
            r.device_cache_unviable = true;
        else if (part == "no_device_cache")
            r.no_device_cache_unviable = true;
        else if (part == "wide")
            r.wide_unviable = true;
        else if (part == "narrow")
            r.narrow_unviable = true;
        else if (part.empty())
            return false;
        // Another name is a later engine's side: read past it, as an unknown key is.
    }
    return true;
}

// A rate measured against another narrow width answers another comparison: a new narrow width
// starts the width's record afresh.
void set_width_narrow(MeasuredKnobs & r, int narrow) {
    if (narrow <= 0 || narrow == r.width_narrow) return;
    r.width_narrow = narrow;
    r.wide_decode_tps = r.narrow_decode_tps = r.wide_prefill_tps = r.narrow_prefill_tps = 0.0;
    r.wide_unviable = r.narrow_unviable = false;
}

} // namespace

std::string format_measured_knobs(const std::vector<MeasuredKnobs> & records) {
    std::string out = std::string(k_header) + "\n";
    char num[32];
    for (const MeasuredKnobs & r : records) {
        out += "hardware=" + r.hardware + " engine=" + r.engine;
        out += " threads=" + std::to_string(r.threads);
        if (!r.threads_tried.empty()) out += " threads_tried=" + ints(r.threads_tried);
        std::snprintf(num, sizeof(num), "%.4f", r.threads_gain);
        out += std::string(" threads_gain=") + num;
        out += " overlap=" + std::to_string(r.overlap);
        out += " overlap_incumbent=" + std::to_string(r.overlap_incumbent);
        std::snprintf(num, sizeof(num), "%.4f", r.overlap_gain);
        out += std::string(" overlap_gain=") + num;
        out += " rounds=" + std::to_string(r.rounds);
        std::snprintf(num, sizeof(num), "%.3f", r.device_cache_tps);
        out += std::string(" device_cache_tps=") + num;
        std::snprintf(num, sizeof(num), "%.3f", r.no_device_cache_tps);
        out += std::string(" no_device_cache_tps=") + num;
        out += " width_narrow=" + std::to_string(r.width_narrow);
        const std::pair<const char *, double> rates[] = {
            {"wide_decode_tps", r.wide_decode_tps},   {"narrow_decode_tps", r.narrow_decode_tps},
            {"wide_prefill_tps", r.wide_prefill_tps}, {"narrow_prefill_tps", r.narrow_prefill_tps},
            {"usage_gen_tokens", r.usage_gen_tokens}, {"usage_long_prompt_tokens", r.usage_long_prompt_tokens}};
        for (const auto & kv : rates) {
            std::snprintf(num, sizeof(num), "%.3f", kv.second);
            out += std::string(" ") + kv.first + "=" + num;
        }
        out += " usage_requests=" + std::to_string(r.usage_requests);
        // The decode's own cost: later keys of the same format, which an older engine reads past.
        std::snprintf(num, sizeof(num), "%.6f", r.decode_compute_s);
        out += std::string(" decode_compute_s=") + num;
        out += " decode_threads=" + std::to_string(r.decode_threads);
        std::snprintf(num, sizeof(num), "%.4f", r.decode_hit);
        out += std::string(" decode_hit=") + num;
        std::snprintf(num, sizeof(num), "%.1f", r.decode_cache_mib);
        out += std::string(" decode_cache_mib=") + num;
        out += " decode_gpu_layers=" + std::to_string(r.decode_gpu_layers);
        out += " decode_device_cache_mb=" + std::to_string(r.decode_device_cache_mb);
        // The request mix: later keys again, read past by an older engine and missing as not recorded.
        out += " mix_requests=" + std::to_string(r.mix_requests);
        const std::pair<const char *, double> mix[] = {{"mix_gen_tokens", r.mix_gen_tokens},
                                                       {"mix_wide_prompt_tokens", r.mix_wide_prompt_tokens},
                                                       {"host_prefill_tps", r.host_prefill_tps}};
        for (const auto & kv : mix) {
            std::snprintf(num, sizeof(num), "%.3f", kv.second);
            out += std::string(" ") + kv.first + "=" + num;
        }
        // Sides that could not be opened: written only when there is one, so a record without any reads
        // as it always did, and an older engine reads past the key.
        std::string unviable;
        for (const auto & kv : unviable_sides(r))
            if (kv.second) unviable += (unviable.empty() ? "" : ",") + std::string(kv.first);
        if (!unviable.empty()) out += " unviable=" + unviable;
        out += "\n";
    }
    return out;
}

bool parse_measured_knobs(const std::string & text, std::vector<MeasuredKnobs> & out, std::string & error) {
    out.clear();
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != k_header) {
        error = "not a measured-knobs record (no '" + std::string(k_header) + "' header)";
        return false;
    }
    int n = 1;
    while (std::getline(in, line)) {
        ++n;
        if (line.empty()) continue;
        MeasuredKnobs r;
        std::istringstream fields(line);
        std::string kv;
        while (fields >> kv) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) {
                error = "line " + std::to_string(n) + ": '" + kv + "' is not key=value";
                return false;
            }
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            auto as_int = [&v](int & dst) {
                char * end = nullptr;
                dst = (int) std::strtol(v.c_str(), &end, 10);
                return !v.empty() && !*end;
            };
            auto as_double = [&v](double & dst) {
                char * end = nullptr;
                dst = std::strtod(v.c_str(), &end);
                return !v.empty() && !*end;
            };
            bool ok = true;
            if (k == "hardware")
                r.hardware = v;
            else if (k == "engine")
                r.engine = v;
            else if (k == "threads")
                ok = as_int(r.threads);
            else if (k == "threads_tried")
                ok = parse_ints(v, r.threads_tried);
            else if (k == "threads_gain")
                ok = as_double(r.threads_gain);
            else if (k == "overlap")
                ok = as_int(r.overlap);
            else if (k == "overlap_incumbent")
                ok = as_int(r.overlap_incumbent);
            else if (k == "overlap_gain")
                ok = as_double(r.overlap_gain);
            else if (k == "rounds")
                ok = as_int(r.rounds);
            else if (k == "device_cache_tps")
                ok = as_double(r.device_cache_tps);
            else if (k == "no_device_cache_tps")
                ok = as_double(r.no_device_cache_tps);
            else if (k == "width_narrow")
                ok = as_int(r.width_narrow);
            else if (k == "wide_decode_tps")
                ok = as_double(r.wide_decode_tps);
            else if (k == "narrow_decode_tps")
                ok = as_double(r.narrow_decode_tps);
            else if (k == "wide_prefill_tps")
                ok = as_double(r.wide_prefill_tps);
            else if (k == "narrow_prefill_tps")
                ok = as_double(r.narrow_prefill_tps);
            else if (k == "usage_gen_tokens")
                ok = as_double(r.usage_gen_tokens);
            else if (k == "usage_long_prompt_tokens")
                ok = as_double(r.usage_long_prompt_tokens);
            else if (k == "usage_requests")
                ok = as_int(r.usage_requests);
            else if (k == "decode_compute_s")
                ok = as_double(r.decode_compute_s);
            else if (k == "decode_threads")
                ok = as_int(r.decode_threads);
            else if (k == "decode_hit")
                ok = as_double(r.decode_hit);
            else if (k == "decode_cache_mib")
                ok = as_double(r.decode_cache_mib);
            else if (k == "decode_gpu_layers")
                ok = as_int(r.decode_gpu_layers);
            else if (k == "decode_device_cache_mb")
                ok = as_int(r.decode_device_cache_mb);
            else if (k == "mix_requests")
                ok = as_int(r.mix_requests);
            else if (k == "mix_gen_tokens")
                ok = as_double(r.mix_gen_tokens);
            else if (k == "mix_wide_prompt_tokens")
                ok = as_double(r.mix_wide_prompt_tokens);
            else if (k == "host_prefill_tps")
                ok = as_double(r.host_prefill_tps);
            else if (k == "unviable")
                ok = parse_unviable(v, r);
            // An unknown key is a later engine's addition: read past it, the record still holds.
            if (!ok) {
                error = "line " + std::to_string(n) + ": bad value for " + k;
                return false;
            }
        }
        if (r.hardware.empty() || r.engine.empty()) {
            error = "line " + std::to_string(n) + ": a record names its machine and engine";
            return false;
        }
        if (r.overlap < -1 || r.overlap > 1 || r.overlap_incumbent < -1 || r.overlap_incumbent > 1 || r.threads < 0 ||
            !(r.device_cache_tps >= 0.0) || !(r.no_device_cache_tps >= 0.0) || r.width_narrow < 0 ||
            !(r.wide_decode_tps >= 0.0) || !(r.narrow_decode_tps >= 0.0) || !(r.wide_prefill_tps >= 0.0) ||
            !(r.narrow_prefill_tps >= 0.0) || !(r.usage_gen_tokens >= 0.0) || !(r.usage_long_prompt_tokens >= 0.0) ||
            r.usage_requests < 0 || !(r.decode_compute_s >= 0.0) || r.decode_threads < 0 ||
            !(r.decode_hit >= -1.0 && r.decode_hit <= 1.0) || !(r.decode_cache_mib >= 0.0) || r.decode_gpu_layers < 0 ||
            r.decode_device_cache_mb < 0 || r.mix_requests < 0 || !(r.mix_gen_tokens >= 0.0) ||
            !(r.mix_wide_prompt_tokens >= 0.0) || !(r.host_prefill_tps >= 0.0)) {
            error = "line " + std::to_string(n) + ": a value out of range";
            return false;
        }
        out.push_back(std::move(r));
    }
    return true;
}

void keep_measured_knobs(std::vector<MeasuredKnobs> & records, const MeasuredKnobs & rec) {
    for (MeasuredKnobs & r : records)
        if (r.hardware == rec.hardware && r.engine == rec.engine) {
            if (rec.threads > 0) {
                r.threads = rec.threads;
                r.threads_tried = rec.threads_tried;
                r.threads_gain = rec.threads_gain;
            }
            if (rec.overlap >= 0) {
                r.overlap = rec.overlap;
                r.overlap_incumbent = rec.overlap_incumbent;
                r.overlap_gain = rec.overlap_gain;
            }
            if (rec.threads > 0 || rec.overlap >= 0) r.rounds = rec.rounds;
            // A side that opened and was timed is viable after all; one that could not open stays marked
            // until a load times it.
            if (rec.device_cache_tps > 0.0) {
                r.device_cache_tps = rec.device_cache_tps;
                r.device_cache_unviable = false;
            }
            if (rec.no_device_cache_tps > 0.0) {
                r.no_device_cache_tps = rec.no_device_cache_tps;
                r.no_device_cache_unviable = false;
            }
            r.device_cache_unviable = r.device_cache_unviable || rec.device_cache_unviable;
            r.no_device_cache_unviable = r.no_device_cache_unviable || rec.no_device_cache_unviable;
            // A different narrow width is a different comparison: what was measured against the old
            // one no longer answers it.
            set_width_narrow(r, rec.width_narrow);
            if (rec.wide_decode_tps > 0.0) {
                r.wide_decode_tps = rec.wide_decode_tps;
                r.wide_unviable = false;
            }
            if (rec.narrow_decode_tps > 0.0) {
                r.narrow_decode_tps = rec.narrow_decode_tps;
                r.narrow_unviable = false;
            }
            r.wide_unviable = r.wide_unviable || rec.wide_unviable;
            r.narrow_unviable = r.narrow_unviable || rec.narrow_unviable;
            if (rec.wide_prefill_tps > 0.0) r.wide_prefill_tps = rec.wide_prefill_tps;
            if (rec.narrow_prefill_tps > 0.0) r.narrow_prefill_tps = rec.narrow_prefill_tps;
            if (rec.usage_requests > 0) {
                r.usage_requests = rec.usage_requests;
                r.usage_gen_tokens = rec.usage_gen_tokens;
                r.usage_long_prompt_tokens = rec.usage_long_prompt_tokens;
            }
            // The decode's cost is one measurement of one configuration: the latest replaces it whole,
            // so a figure is never paired with the thread count or the cache of another load.
            if (rec.decode_compute_s > 0.0 || rec.decode_hit >= 0.0) {
                r.decode_compute_s = rec.decode_compute_s;
                r.decode_threads = rec.decode_threads;
                r.decode_hit = rec.decode_hit;
                r.decode_cache_mib = rec.decode_cache_mib;
                r.decode_gpu_layers = rec.decode_gpu_layers;
                r.decode_device_cache_mb = rec.decode_device_cache_mb;
            }
            // The mix is the latest load's that served a request, whole: a front-end hands on a load's
            // record each time it grows, so the figures are already that load's running means, and
            // adding them to the stored ones would count the same requests again. The host's prefill
            // rate is kept from an earlier load where this one ran every long prompt on a device.
            if (rec.mix_requests > 0) {
                r.mix_requests = rec.mix_requests;
                r.mix_gen_tokens = rec.mix_gen_tokens;
                r.mix_wide_prompt_tokens = rec.mix_wide_prompt_tokens;
            }
            if (rec.host_prefill_tps > 0.0) r.host_prefill_tps = rec.host_prefill_tps;
            return;
        }
    records.push_back(rec);
}

void record_side_rate(const KnobTrialConfig & trial, double tokens_per_second, MeasuredKnobs & rec) {
    if (!(tokens_per_second > 0.0)) return;
    if (trial.device_tier >= 0) {
        (trial.device_tier == 1 ? rec.device_cache_tps : rec.no_device_cache_tps) = tokens_per_second;
        (trial.device_tier == 1 ? rec.device_cache_unviable : rec.no_device_cache_unviable) = false;
    } else if (trial.width_side >= 0) {
        set_width_narrow(rec, trial.width_narrow);
        (trial.width_side == 1 ? rec.wide_decode_tps : rec.narrow_decode_tps) = tokens_per_second;
        (trial.width_side == 1 ? rec.wide_unviable : rec.narrow_unviable) = false;
    }
}

void record_side_unviable(const KnobTrialConfig & trial, MeasuredKnobs & rec) {
    if (trial.device_tier >= 0) {
        (trial.device_tier == 1 ? rec.device_cache_unviable : rec.no_device_cache_unviable) = true;
    } else if (trial.width_side >= 0) {
        set_width_narrow(rec, trial.width_narrow);
        (trial.width_side == 1 ? rec.wide_unviable : rec.narrow_unviable) = true;
    }
}

const MeasuredKnobs * find_measured_knobs(const std::vector<MeasuredKnobs> & records,
                                          const std::string & hardware,
                                          const std::string & engine) {
    for (const MeasuredKnobs & r : records)
        if (r.hardware == hardware && r.engine == engine) return &r;
    return nullptr;
}

} // namespace bmoe
