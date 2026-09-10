#include "json_util.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace bmoe::server {

bool value_to_param_string(const json & v, std::string & out) {
    if (v.is_boolean()) {
        out = v.get<bool>() ? "true" : "false";
        return true;
    }
    if (v.is_number_integer()) {
        out = std::to_string(v.get<long long>());
        return true;
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        // An integral float (a form that sent 4.0) is still a valid Int parameter value.
        if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9e15) {
            out = std::to_string((long long) d);
            return true;
        }
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.9g", d);
        out = buf;
        return true;
    }
    if (v.is_string()) {
        out = v.get<std::string>();
        return true;
    }
    return false;
}

json param_string_to_value(const ParamDesc & d, const std::string & s) {
    switch (d.type) {
    case ParamType::Bool:
        return s == "true";
    case ParamType::Int:
        if (s == "auto") return s;
        return std::strtoll(s.c_str(), nullptr, 10);
    case ParamType::Float:
        return std::strtod(s.c_str(), nullptr);
    case ParamType::Choice:
    case ParamType::Path:
        break;
    }
    return s;
}

json config_values(const RunConfig & cfg) {
    json o = json::object();
    for (const ParamDesc & d : params())
        o[d.key] = param_string_to_value(d, d.get(cfg));
    return o;
}

std::string utf8_as_sent(const std::string & s) {
    return json::parse(json(s).dump(-1, ' ', false, json::error_handler_t::replace)).get<std::string>();
}

static double mib(uint64_t bytes) {
    return bytes / (1024.0 * 1024.0);
}

json token_json(const TokenMetrics & m) {
    return json{{"step", m.step},
                {"steps", m.steps},
                {"wall_ms", m.wall_ms},
                {"io_ms", m.io_ms},
                {"compute_ms", m.compute_ms},
                {"mgmt_ms", m.mgmt_ms},
                {"stall_ms", m.stall_ms},
                {"read_mib", mib(m.read_bytes)},
                {"cache_hit_pct", m.cache_hit_pct},
                {"cache_budget_mib", m.cache_budget_mib},
                {"rss_mib", m.rss_mib},
                {"mem_available_mib", m.mem_available_mib},
                {"majflt", m.majflt},
                {"cpu_ms", m.cpu_ms},
                {"dense_resident_frac", m.dense_resident_frac},
                {"mtp_batch", m.mtp_batch}};
}

json summary_json(const RunSummary & s, bool cancelled) {
    return json{{"cancelled", cancelled},
                {"tokens", s.n_generated},
                {"tok_s", s.tokens_per_second},
                {"prefill_s", s.prefill_seconds},
                {"prefill_tps", s.prefill_seconds > 0 ? s.n_prompt / s.prefill_seconds : 0.0},
                {"n_prompt", s.n_prompt},
                {"n_past", s.n_past},
                {"cache_hit_pct", s.cache_hit_pct},
                {"read_mib", s.moe_read_mib},
                {"io_s_tok", s.moe_io_s_per_token},
                {"compute_s_tok", s.moe_compute_s_per_token},
                {"stall_s_tok", s.moe_stall_s_per_token},
                {"mgmt_s_tok", s.moe_mgmt_s_per_token},
                {"cache_resident_mib", s.cache_resident_mib},
                {"cache_budget_mib", s.cache_budget_mib},
                {"ttft_s", s.prefill_seconds}};
}

} // namespace bmoe::server
