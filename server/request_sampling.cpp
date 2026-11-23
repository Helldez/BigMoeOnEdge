#include "request_sampling.h"

#include <cmath>
#include <limits>

namespace bmoe::server {

namespace {

bool present(const json & body, const char * key) {
    return body.contains(key) && !body[key].is_null();
}

// A finite number in [lo, hi], or the field is refused.
bool read_number(
    const json & body, const char * key, double lo, double hi, std::optional<double> & out, std::string & err) {
    if (!present(body, key)) return true;
    const json & v = body[key];
    if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < lo || v.get<double>() > hi) {
        err = std::string(key) + " must be a number from " + json(lo).dump() + " to " + json(hi).dump();
        return false;
    }
    out = v.get<double>();
    return true;
}

bool read_integer(const json & body,
                  const char * key,
                  long long lo,
                  long long hi,
                  std::optional<long long> & out,
                  std::string & err) {
    if (!present(body, key)) return true;
    const json & v = body[key];
    if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<long long>() < lo) ||
        (v.is_number_unsigned() && v.get<unsigned long long>() > (unsigned long long) hi) ||
        (!v.is_number_unsigned() && v.get<long long>() > hi)) {
        err = std::string(key) + " must be an integer from " + std::to_string(lo) + " to " + std::to_string(hi);
        return false;
    }
    out = v.get<long long>();
    return true;
}

// stop / stop_sequences: one string or a list of them. Empty strings stop nothing and are dropped.
bool read_stop(const json & body, const char * key, std::vector<std::string> & out, std::string & err) {
    if (!present(body, key)) return true;
    const json & v = body[key];
    if (v.is_string()) {
        if (!v.get<std::string>().empty()) out.push_back(v.get<std::string>());
        return true;
    }
    if (!v.is_array()) {
        err = std::string(key) + " must be a string or an array of strings";
        return false;
    }
    for (const json & s : v) {
        if (!s.is_string()) {
            err = std::string(key) + " must be a string or an array of strings";
            return false;
        }
        if (!s.get<std::string>().empty()) out.push_back(s.get<std::string>());
    }
    return true;
}

// One bias: a number, or false for "never this token" (llama-server's spelling).
bool bias_value(const json & v, float & out) {
    if (v == json(false)) {
        out = -std::numeric_limits<float>::infinity();
        return true;
    }
    if (!v.is_number() || !std::isfinite(v.get<double>())) return false;
    out = (float) v.get<double>();
    return true;
}

bool token_id(const std::string & key, int32_t & out) {
    if (key.empty() || key.size() > 10) return false;
    long long id = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return false;
        id = id * 10 + (c - '0');
    }
    if (id > std::numeric_limits<int32_t>::max()) return false;
    out = (int32_t) id;
    return true;
}

// logit_bias: OpenAI's object of token id to bias, or llama-server's list of [id, bias] pairs.
// A token named by its text, which llama-server also reads, is refused: ids only.
bool read_logit_bias(const json & body, std::vector<std::pair<int32_t, float>> & out, std::string & err) {
    if (!present(body, "logit_bias")) return true;
    const json & v = body["logit_bias"];
    const std::string what = "logit_bias must map token ids to numbers (or false), as an object or [id, bias] pairs";
    if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it) {
            int32_t id = 0;
            float b = 0.0f;
            if (!token_id(it.key(), id) || !bias_value(it.value(), b)) {
                err = what;
                return false;
            }
            out.push_back({id, b});
        }
        return true;
    }
    if (!v.is_array()) {
        err = what;
        return false;
    }
    for (const json & pair : v) {
        float b = 0.0f;
        if (!pair.is_array() || pair.size() != 2 || !pair[0].is_number_integer() || pair[0].get<long long>() < 0 ||
            pair[0].get<long long>() > std::numeric_limits<int32_t>::max() || !bias_value(pair[1], b)) {
            err = what;
            return false;
        }
        out.push_back({(int32_t) pair[0].get<long long>(), b});
    }
    return true;
}

} // namespace

bool parse_request_sampling(const json & body, SamplingDialect dialect, ChatRequest & turn, std::string & err) {
    const bool chat = dialect == SamplingDialect::Chat;
    RequestSampling r;
    bool any = false;
    std::optional<double> d;
    std::optional<long long> i;

    // OpenAI bounds temperature to 2 and Anthropic to 1; llama-server takes any non-negative one.
    if (!read_number(body, "temperature", 0.0, 100.0, d, err)) return false;
    if (d) r.temp = (float) *d, any = true;
    d.reset();
    if (!read_number(body, "top_p", 0.0, 1.0, d, err)) return false;
    if (d) r.top_p = (float) *d, any = true;
    d.reset();
    if (dialect != SamplingDialect::Responses) {
        if (!read_integer(body, "top_k", 0, std::numeric_limits<int32_t>::max(), i, err)) return false;
        if (i) r.top_k = (int) *i, any = true;
        i.reset();
    }
    if (chat) {
        if (!read_number(body, "min_p", 0.0, 1.0, d, err)) return false;
        if (d) r.min_p = (float) *d, any = true;
        d.reset();
        // -1 is llama-server's "a new seed each time", the session's own default.
        if (!read_integer(body, "seed", -1, std::numeric_limits<uint32_t>::max(), i, err)) return false;
        if (i) r.seed = *i < 0 ? SamplingConfig::seed_random : (uint32_t) *i, any = true;
        i.reset();
        if (!read_number(body, "presence_penalty", -SamplingConfig::penalty_max, SamplingConfig::penalty_max, d, err))
            return false;
        if (d) r.presence_penalty = (float) *d, any = true;
        d.reset();
        if (!read_number(body, "frequency_penalty", -SamplingConfig::penalty_max, SamplingConfig::penalty_max, d, err))
            return false;
        if (d) r.frequency_penalty = (float) *d, any = true;
        d.reset();
        if (!read_number(body, "repeat_penalty", SamplingConfig::repeat_penalty_min, SamplingConfig::repeat_penalty_max,
                         d, err))
            return false;
        if (d) r.repeat_penalty = (float) *d, any = true;
        d.reset();
        if (!read_integer(body, "repeat_last_n", SamplingConfig::penalty_last_n_ctx,
                          std::numeric_limits<int32_t>::max(), i, err))
            return false;
        if (i) r.penalty_last_n = (int) *i, any = true;
        i.reset();
        if (!read_logit_bias(body, r.logit_bias, err)) return false;
        if (!r.logit_bias.empty()) any = true;
    }
    if (any) turn.sampling = r;

    if (dialect == SamplingDialect::Messages) {
        if (!read_stop(body, "stop_sequences", turn.stop_text, err)) return false;
    } else if (chat) {
        if (!read_stop(body, "stop", turn.stop_text, err)) return false;
    }

    if (chat) {
        if (present(body, "grammar")) {
            if (!body["grammar"].is_string()) {
                err = "grammar must be a string (GBNF)";
                return false;
            }
            turn.grammar = body["grammar"].get<std::string>();
        }
        if (!turn.grammar.empty() && (!turn.tools.empty() || !turn.json_schema.empty())) {
            err = "grammar cannot be combined with tools or a response format, which build their own";
            return false;
        }
        if (present(body, "logprobs")) {
            if (!body["logprobs"].is_boolean()) {
                err = "logprobs must be a boolean";
                return false;
            }
            turn.logprobs = body["logprobs"].get<bool>();
        }
        if (!read_integer(body, "top_logprobs", 0, k_max_top_logprobs, i, err)) return false;
        if (i) turn.top_logprobs = (int) *i;
        i.reset();
        // llama-server's own spelling of the same thing: n_probs candidates, logprobs implied.
        if (!read_integer(body, "n_probs", 0, k_max_top_logprobs, i, err)) return false;
        if (i && *i > 0) turn.logprobs = true, turn.top_logprobs = std::max(turn.top_logprobs, (int) *i);
        i.reset();
        if (!read_integer(body, "n", 1, std::numeric_limits<int32_t>::max(), i, err)) return false;
        if (i && *i != 1) {
            err = "n must be 1: this server answers one choice per request";
            return false;
        }
    }
    return true;
}

} // namespace bmoe::server
