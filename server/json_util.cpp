#include "json_util.h"

#include <algorithm>
#include <cctype>
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
    case ParamType::Text:
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

bool is_json_content_type(const std::string & content_type) {
    static const std::string want = "application/json";
    const auto blank = [](char c) { return c == ' ' || c == '\t'; };
    size_t b = 0, e = std::min(content_type.find(';'), content_type.size());
    while (b < e && blank(content_type[b]))
        ++b;
    while (e > b && blank(content_type[e - 1]))
        --e;
    if (e - b != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::tolower((unsigned char) content_type[b + i]) != want[i]) return false;
    return true;
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
                {"flash_wait_ms", m.flash_wait_ms},
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

// The keys are the CLI's BMOE_DONE keys (docs/telemetry.md), so a metrics view reads the two
// front-ends alike, plus the facts a UI would otherwise derive from the configuration: the flash
// term that is a share of the wall (flash_wait_*), already chosen for the run's mode.
json summary_json(const RunSummary & s, bool cancelled) {
    return json{{"cancelled", cancelled},
                {"tokens", s.n_generated},
                {"tok_s", s.tokens_per_second},
                {"load_s", s.load_seconds},
                {"prefill_s", s.prefill_seconds},
                {"prefill_tps", s.prefill_seconds > 0 ? s.n_prompt / s.prefill_seconds : 0.0},
                {"prefill_read_mib", s.prefill_read_mib},
                {"prefill_dev_tokens", s.prefill_device_tokens},
                {"n_prompt", s.n_prompt},
                {"n_past", s.n_past},
                {"cache_hit_pct", s.cache_hit_pct},
                {"read_mib", s.moe_read_mib},
                {"io_s_tok", s.moe_io_s_per_token},
                {"compute_s_tok", s.moe_compute_s_per_token},
                {"stall_s_tok", s.moe_stall_s_per_token},
                {"flash_wait_s_tok", s.moe_flash_wait_s_per_token},
                {"mgmt_s_tok", s.moe_mgmt_s_per_token},
                {"majflt_tok", s.majflt_per_token},
                {"cpu_s_tok", s.cpu_s_per_token},
                {"loop_overhead_s_tok", s.loop_overhead_s_per_token},
                {"cache_resident_mib", s.cache_resident_mib},
                {"cache_budget_mib", s.cache_budget_mib},
                {"cache_evictions", s.cache_evictions},
                {"cache_rereads", s.cache_rereads},
                {"token_demand_mib", s.token_demand_mib},
                {"mtp_drafted", s.mtp_drafted},
                {"mtp_accepted", s.mtp_accepted},
                {"mtp_decodes", s.mtp_decodes},
                {"drafted_steps", s.drafted_steps},
                {"mtp_draft_s_tok", s.mtp_draft_s_per_token},
                {"ttft_s", s.prefill_seconds}};
}

} // namespace bmoe::server
