#include "decide_protocol.h"

#include <cmath>

namespace bmoe::server {

bool parse_decide_request(const json & body, DecideRequest & out, std::string & err) {
    if (!body.is_object()) {
        err = "the body must be a JSON object";
        return false;
    }
    const json & prefix = body.value("prefix", json(""));
    const json & suffix = body.value("suffix", json(""));
    if (!prefix.is_string() || !suffix.is_string()) {
        err = "prefix and suffix must be strings";
        return false;
    }
    if (!body.contains("choices") || !body["choices"].is_array()) {
        err = "choices must be an array of strings";
        return false;
    }
    std::vector<std::string> choices;
    for (const json & c : body["choices"]) {
        if (!c.is_string()) {
            err = "choices must be an array of strings";
            return false;
        }
        choices.push_back(c.get<std::string>());
    }
    if (body.contains("reuse_prefix") && !body["reuse_prefix"].is_boolean()) {
        err = "reuse_prefix must be a boolean";
        return false;
    }
    out.prefix = prefix.get<std::string>();
    out.suffix = suffix.get<std::string>();
    out.choices = std::move(choices);
    out.reuse_prefix = body.value("reuse_prefix", true);
    return true;
}

json decide_json(const DecideResult & r) {
    // A choice the model gives no mass to is -inf, which JSON cannot carry: null, as the CLI sends.
    json logp = json::array();
    for (double v : r.choice_logp)
        logp.push_back(std::isfinite(v) ? json(v) : json(nullptr));
    const PrefillStats & p = r.prefill;
    return json{{"cancelled", r.cancelled},
                {"best", r.best},
                {"choice_logp", logp},
                {"n_tokens", r.n_tokens},
                {"n_reused", r.n_reused},
                {"n_prefilled", r.n_prefilled},
                {"restore_s", r.restore_seconds},
                {"store_s", r.store_seconds},
                {"prefill_s", p.seconds},
                {"prefill_cpu_s", p.cpu_seconds},
                {"prefill_read_mib", p.read_mib},
                {"prefill_io_s", p.io_seconds},
                {"prefill_stall_s", p.stall_seconds},
                {"prefill_mgmt_s", p.mgmt_seconds},
                {"prefill_dev_tokens", p.device_tokens},
                {"prefill_dev_read_mib", p.device_read_mib},
                {"prefill_dev_stall_s", p.device_stall_seconds},
                {"prefill_dev_routed", p.device_routed},
                {"prefill_dev_demand", p.device_demand},
                {"prefix_state_mib", r.prefix_state_bytes / (1024.0 * 1024.0)}};
}

} // namespace bmoe::server
