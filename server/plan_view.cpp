#include "plan_view.h"

#include "bmoe/allocate.h"
#include "bmoe/params.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace bmoe::server {
namespace {

std::string sentence_case(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = (char) (s[0] - 'a' + 'A');
    return s;
}

// What a plan says at a glance. Built from the regime and the allocation alone, so it reads the
// same on every machine.
std::string summary_sentence(const Plan & plan) {
    if (plan.streaming_declined)
        return plan.decline_reason.empty() ? "Streaming declined." : "Streaming declined: " + plan.decline_reason;
    switch (plan.regime) {
    case Regime::ExpertsStream:
        return plan.allocation.cache_bytes == 0
                   ? "Experts stream from flash, without a cache."
                   : "Experts stream from flash, with a " + size_brief(plan.allocation.cache_bytes) + " cache.";
    case Regime::Fits:
        return "The whole model fits in memory: nothing streams.";
    case Regime::DenseOversized:
        return "Not even the dense weights fit in memory.";
    case Regime::NotMoe:
        return "Not a mixture-of-experts model: there is nothing to stream.";
    case Regime::Unknown:
        break;
    }
    return "The model could not be read.";
}

// A device's memory is its own only when that was measured; an unknown is shared, as the planner
// reads it.
bool own_memory(const ComputeDevice & d) {
    return !d.is_cpu && d.shares_host_memory == Tri::No;
}

bool has_layers(const Plan & plan, const ComputeDevice & d) {
    const Placement & pl = plan.placement;
    if (!pl.fitted || pl.shared_memory_placement || pl.device_bytes == 0 || !own_memory(d)) return false;
    return std::find(pl.projected_devices.begin(), pl.projected_devices.end(), d.name) != pl.projected_devices.end();
}

std::string host_name(const HardwareProfile & hw) {
    for (const ComputeDevice & d : hw.devices)
        if (d.is_cpu) return d.name;
    return "the host";
}

// The badge a device row carries: what it does, or why it does nothing.
std::string device_badge(const std::vector<std::string> & uses, const std::string & status) {
    if (status == "refused") return "Wrong results";
    if (status == "unusable") return "Unusable";
    if (uses.empty()) return "Not used";
    std::string s = uses[0];
    for (size_t i = 1; i < uses.size(); ++i)
        s += (i + 1 == uses.size() ? " and " : ", ") + uses[i];
    return sentence_case(s);
}

std::string rate_text(const ComputeDevice & d, double prompt_ratio) {
    if (d.is_cpu) return "The reference";
    if (!(prompt_ratio > 0)) return "Not measured on a prompt";
    char b[64];
    std::snprintf(b, sizeof b, prompt_ratio >= 10 ? "%.0fx the host on a prompt" : "%.1fx the host on a prompt",
                  prompt_ratio);
    return b;
}

// Every device the plan saw, with what it does in this run and why, from the facts the rules read.
json devices_json(const HardwareProfile & hw, const Plan & plan) {
    const std::string & armed = plan.config.prefill.device;
    json out = json::array();
    for (const ComputeDevice & d : hw.devices) {
        std::vector<std::string> uses;
        std::string status = "unused", why;
        const double prompt_ratio = d.is_cpu ? 1.0
                                    : (hw.host_wide_matmul_gibs > 0 && d.wide_matmul_gibs > 0)
                                        ? d.wide_matmul_gibs / hw.host_wide_matmul_gibs
                                        : 0.0;
        const double token_ratio = d.is_cpu ? 1.0
                                   : (hw.host_bandwidth_gibs > 0 && d.memory_bandwidth_gibs > 0)
                                       ? d.memory_bandwidth_gibs / hw.host_bandwidth_gibs
                                       : 0.0;
        if (d.is_cpu) {
            if (armed.empty()) uses.push_back("prompt");
            uses.push_back("tokens");
            why = "The host: every rate is measured against it.";
        } else if (d.usable == Tri::No) {
            status = "unusable";
            why = d.unusable_reason.empty() ? "Its probe did not come back, so nothing uses it." : d.unusable_reason;
        } else {
            if (has_layers(plan, d)) uses.push_back("layers");
            if (d.name == armed) uses.push_back("prompt");
            if (!uses.empty()) {
                why = d.name == armed ? "Faster than the host on a prompt, with the same results."
                                      : "Holds layers in memory of its own.";
            } else if (d.identity_ok == Tri::No || d.wide_identity_ok == Tri::No) {
                status = "refused";
                why = "Its results did not match the host's, so it is never used.";
            } else if (prompt_ratio == 0) {
                why = "Not measured at prompt width, so the plan does not arm it.";
            } else {
                why = armed.empty() ? "The prompt stays on the host: the prefill decision says why."
                                    : armed + " is the one the plan arms for the prompt.";
            }
        }
        if (!uses.empty()) status = "used";
        out.push_back({{"name", d.name},
                       {"description", d.description},
                       {"cpu", d.is_cpu},
                       {"own_memory", own_memory(d)},
                       {"memory_total", d.memory_total},
                       {"prompt_ratio", prompt_ratio},
                       {"token_ratio", token_ratio},
                       {"rate_text", rate_text(d, prompt_ratio)},
                       {"uses", uses},
                       {"status", status},
                       {"badge", device_badge(uses, status)},
                       {"why", why}});
    }
    return out;
}

// Why the list holds no device beyond the host, when it does not: three answers that must not read
// as one (HardwareProfile::backends_placeable).
std::string devices_none(const HardwareProfile & hw) {
    for (const ComputeDevice & d : hw.devices)
        if (!d.is_cpu) return "";
    if (!hw.backends_looked) return "not_probed";
    return hw.backends_placeable > 0 ? "machine" : "build";
}

std::string devices_none_text(const std::string & code) {
    if (code == "build") return "No other device: this build carries no backend for one.";
    if (code == "machine") return "No other device: the backends this build carries found none here.";
    if (code == "not_probed") return "No other device was looked for.";
    return "";
}

json row_json(const std::string & name, uint64_t bytes, bool locked, bool in_turn) {
    return {{"name", name}, {"label", sentence_case(name)}, {"bytes", bytes}, {"locked", locked}, {"in_turn", in_turn}};
}

// Where the run's memory goes: the ledger the planner charged, one pool per memory that is
// separate. The host's pool is the ledger; a device with memory of its own that holds layers is a
// second pool, from the placement.
json memory_json(const HardwareProfile & hw, const Plan & plan) {
    const Ledger & l = plan.allocation.ledger;
    json rows = json::array();
    for (const LedgerRow & r : l.rows)
        rows.push_back(row_json(r.name, r.bytes, r.locked, r.in_turn));
    json pools = json::array();
    pools.push_back({{"name", "host"},
                     {"label", "This process"},
                     {"cap_bytes", l.holdable_cap},
                     {"lockable_cap_bytes", l.lockable_cap},
                     {"lockable_cap_known", l.lockable_cap_known},
                     {"cache_lock_unverified", plan.allocation.cache_lock_unverified},
                     {"held_bytes", l.held},
                     {"locked_bytes", l.locked},
                     {"rows", rows}});
    const Placement & pl = plan.placement;
    std::vector<const ComputeDevice *> placed;
    for (const ComputeDevice & d : hw.devices)
        if (has_layers(plan, d)) placed.push_back(&d);
    for (const ComputeDevice * d : placed) {
        // The placement reports one device's compute buffers by name and its weights and context
        // only summed over the devices it placed on. With one device that sum is that device's;
        // with several it is shared out evenly, and the pool says so.
        const uint64_t n = placed.size();
        const uint64_t compute = pl.device_compute_of(d->name);
        const uint64_t fixed = pl.device_compute_bytes + pl.device_context_bytes;
        const uint64_t weights = (pl.device_bytes > fixed ? pl.device_bytes - fixed : 0) / n;
        const uint64_t context = pl.device_context_bytes / n;
        json drows = json::array();
        if (weights) drows.push_back(row_json("layers", weights, false, false));
        if (compute + context) drows.push_back(row_json("context and compute", compute + context, false, false));
        pools.push_back({{"name", d->name},
                         {"label", d->name},
                         {"cap_bytes", d->memory_total},
                         {"lockable_cap_bytes", 0},
                         {"held_bytes", weights + compute + context},
                         {"locked_bytes", 0},
                         {"estimated", n > 1},
                         {"rows", drows}});
    }
    return json{{"pools", pools}};
}

// Which phase runs where: the lanes a person asks about first.
json lanes_json(const HardwareProfile & hw, const Plan & plan) {
    const std::string host = host_name(hw);
    json tokens = json::array({host});
    for (const ComputeDevice & d : hw.devices)
        if (has_layers(plan, d)) tokens.push_back(d.name);
    const std::string & armed = plan.config.prefill.device;
    return json::array(
        {json{{"phase", "prompt"}, {"label", "Prompt"}, {"on", json::array({armed.empty() ? host : armed})}},
         json{{"phase", "tokens"}, {"label", "Tokens"}, {"on", tokens}}});
}

// The three groups a person reads a plan in, by who decided and what: a setting the plan decided
// from a fact (whether or not it differs from the value it had), a setting it left to the caller or
// could not measure, and the notes that are not settings at all (the regime, the ledger, each of the
// cost model's notes under its topic).
const char * decision_group(const Decision & d, bool is_param) {
    if (!is_param) return "notes";
    if (d.source == Source::Operator || d.source == Source::Unprobed) return "kept";
    return "set";
}

// The title a note is read under. A note is no parameter, so the settings table has no label for
// it; the names live here, once, so no UI keeps a table of its own. A topic this does not know is
// titled from its key, which keeps a new note readable before it is named.
std::string note_label(const std::string & knob) {
    static const struct {
        const char * knob;
        const char * label;
    } k_labels[] = {
        {"regime", "Streaming mode"},
        {"residency", "Residency"},
        {"placement", "Layer placement"},
        {"placement-cost", "Placement cost"},
        {"placement-declined", "Placement declined"},
        {"extra-offload", "Further offload"},
        {"experts", "Where experts live"},
        {"memory-ledger", "Memory use"},
        {"cache-floor", "Cache floor"},
        {"cache-off", "Cache off"},
        {"cache-lock", "Cache lock"},
        {"cache-unpriced", "Cache size"},
        {"kv-budget", "Context memory"},
        {"lossy", "Output quality"},
        {"shared-pool", "Shared memory pool"},
        {"shared-memory", "Shared memory"},
        {"storage-rate", "Storage speed"},
        {"storage-spread", "Storage consistency"},
        {"unmeasured-compute", "Compute speed"},
        {"unmeasured-refault", "Re-read cost"},
        {"unmeasured-storage", "Storage speed"},
        {"dense-pin", "Dense weights pinning"},
        {"device-crossing", "Device hop"},
        {"device-unapplied", "Device placement"},
        {"device-excluded", "Device excluded"},
        {"device-host-pointer", "Device memory access"},
    };
    for (const auto & l : k_labels)
        if (knob == l.knob) return l.label;
    std::string words = knob;
    std::replace(words.begin(), words.end(), '-', ' ');
    return sentence_case(words);
}

} // namespace

json plan_view(const HardwareProfile & hw, const ModelProfile & model, const Plan & plan) {
    // Decode as the plan priced it, the figures bmoe-cli --auto prints after a run, so a client can
    // set a load's measured figures beside them.
    json predicted = json::object();
    if (plan.allocation.seconds_per_token > 0) predicted["s_per_token"] = plan.allocation.seconds_per_token;
    if (plan.allocation.cache_bytes > 0 && model.expert_bytes > 0)
        predicted["cache_hit_pct"] = 100.0 * (double) plan.allocation.cache_bytes / (double) model.expert_bytes;
    const std::string none = devices_none(hw);
    return json{{"summary", {{"sentence", summary_sentence(plan)}, {"lanes", lanes_json(hw, plan)}}},
                {"predicted", predicted},
                {"memory", memory_json(hw, plan)},
                {"devices", devices_json(hw, plan)},
                {"devices_none", none},
                {"devices_none_text", devices_none_text(none)}};
}

std::string value_text(const Decision & d) {
    const std::string & v = d.value;
    if (v.empty() || v == "-") return "";
    char * end = nullptr;
    const double n = std::strtod(v.c_str(), &end);
    const bool numeric = end != v.c_str();
    const std::string rest = numeric ? std::string(end) : v;
    if (const ParamDesc * p = find_param(d.knob)) {
        for (const ParamChoice & c : p->choices)
            if (c.value == v) return c.label;
        if (v == "true") return "on";
        if (v == "false") return "off";
        if (numeric && rest.empty() && p->unit == "MiB") return size_brief((uint64_t) (n * 1048576.0));
        if (numeric && rest.empty() && !p->unit.empty()) return v + " " + p->unit;
        return v;
    }
    if (numeric && rest == " MiB") return size_brief((uint64_t) (n * 1048576.0));
    // A note's value that is a word of the planner's ("experts-stream") reads as words.
    if (!numeric &&
        std::all_of(v.begin(), v.end(), [](char c) { return std::isalpha((unsigned char) c) || c == '-'; })) {
        std::string w = v;
        std::replace(w.begin(), w.end(), '-', ' ');
        return w;
    }
    return v;
}

json decision_json(const Decision & d) {
    const ParamDesc * p = find_param(d.knob);
    return {{"knob", d.knob},
            {"label", p ? p->label : note_label(d.knob)},
            {"group", decision_group(d, p != nullptr)},
            {"value", d.value},
            {"value_text", value_text(d)},
            {"source", source_name(d.source)},
            {"brief", d.brief.empty() ? d.reason : d.brief},
            {"reason", d.reason}};
}

std::string plan_step_label(const std::string & step) {
    if (step == "devices") return "Compute devices";
    if (step == "model") return "The model";
    if (step == "compute") return "Compute rates";
    if (step == "storage") return "Storage";
    if (step == "memory") return "Memory";
    if (step == "placement") return "Placement";
    if (step == "rules") return "Decisions";
    return sentence_case(step);
}

} // namespace bmoe::server
