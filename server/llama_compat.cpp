#include "llama_compat.h"

#include <algorithm>
#include <sstream>

namespace bmoe::server {

namespace {

// The whole prompt the answer saw: what the turn decoded plus what the context held (usage_json's rule).
int prompt_seen(const RunSummary & s) {
    return s.n_past - s.n_generated > s.n_prompt ? s.n_past - s.n_generated : s.n_prompt;
}

double per_second(double n, double seconds) {
    return seconds > 0.0 ? n / seconds : 0.0;
}

} // namespace

json timings_json(const RunSummary & s) {
    const double prompt_ms = s.prefill_seconds * 1e3, predicted_ms = s.gen_seconds * 1e3;
    return {{"cache_n", prompt_seen(s) - s.n_prompt},
            {"prompt_n", s.n_prompt},
            {"prompt_ms", prompt_ms},
            {"prompt_per_token_ms", s.n_prompt > 0 ? prompt_ms / s.n_prompt : 0.0},
            {"prompt_per_second", per_second(s.n_prompt, s.prefill_seconds)},
            {"predicted_n", s.n_generated},
            {"predicted_ms", predicted_ms},
            {"predicted_per_token_ms", s.n_generated > 0 ? predicted_ms / s.n_generated : 0.0},
            {"predicted_per_second", per_second(s.n_generated, s.gen_seconds)}};
}

json prompt_progress_json(const PrefillProgress & p) {
    return {{"total", p.total}, {"cache", p.cached}, {"processed", p.processed}, {"time_ms", p.seconds * 1e3}};
}

void ServeTotals::add(const RunSummary & s) {
    ++turns;
    prompt_tokens += s.n_prompt;
    prompt_cached += prompt_seen(s) - s.n_prompt;
    prompt_seconds += s.prefill_seconds;
    predicted_tokens += s.n_generated;
    predicted_seconds += s.gen_seconds;
    n_tokens_max = std::max(n_tokens_max, s.n_past);
    window_prompt_tokens += s.n_prompt;
    window_prompt_seconds += s.prefill_seconds;
    window_predicted_tokens += s.n_generated;
    window_predicted_seconds += s.gen_seconds;
}

std::string metrics_text(const ServeTotals & t, int processing, int deferred) {
    struct Item {
        const char * name;
        const char * help;
        double value;
    };
    const Item counters[] = {
        {"prompt_tokens_total", "Number of prompt tokens processed, excluding cached tokens", (double) t.prompt_tokens},
        {"prompt_tokens_cached_total", "Number of prompt tokens reused from the cache", (double) t.prompt_cached},
        {"prompt_seconds_total", "Total time spent processing prompts", t.prompt_seconds},
        {"tokens_predicted_total", "Number of generation tokens processed", (double) t.predicted_tokens},
        {"tokens_predicted_seconds_total", "Total time spent generating tokens", t.predicted_seconds},
        {"n_tokens_max", "Largest observed sequence length (prompt + generation)", (double) t.n_tokens_max},
    };
    const Item gauges[] = {
        {"prompt_tokens_seconds", "Average prompt throughput in tokens/s",
         per_second((double) t.window_prompt_tokens, t.window_prompt_seconds)},
        {"predicted_tokens_seconds", "Average generation throughput in tokens/s",
         per_second((double) t.window_predicted_tokens, t.window_predicted_seconds)},
        {"requests_processing", "Number of requests processing", (double) processing},
        {"requests_deferred", "Number of requests deferred", (double) deferred},
    };
    std::ostringstream out;
    const auto write = [&out](const char * type, const Item & i) {
        out << "# HELP llamacpp:" << i.name << " " << i.help << "\n"
            << "# TYPE llamacpp:" << i.name << " " << type << "\n"
            << "llamacpp:" << i.name << " " << i.value << "\n";
    };
    for (const Item & i : counters)
        write("counter", i);
    for (const Item & i : gauges)
        write("gauge", i);
    return out.str();
}

json props_json(const PropsFacts & f) {
    const json params = {{"n_predict", f.n_predict},
                         {"temperature", f.temperature},
                         {"top_k", f.top_k},
                         {"top_p", f.top_p},
                         {"seed", f.seed},
                         {"presence_penalty", f.presence_penalty},
                         {"frequency_penalty", f.frequency_penalty},
                         {"repeat_penalty", f.repeat_penalty},
                         {"repeat_last_n", f.repeat_last_n}};
    return {{"default_generation_settings", {{"params", params}, {"n_ctx", f.n_ctx}}},
            {"total_slots", 1},
            {"model_alias", f.model_name},
            {"model_path", f.model_path},
            {"modalities", {{"vision", f.vision}, {"video", false}, {"audio", false}}},
            {"endpoint_slots", true},
            {"endpoint_props", false},
            {"endpoint_metrics", true},
            {"build_info", f.build},
            {"is_sleeping", false}};
}

json slots_json(int n_ctx, bool processing) {
    return json::array({{{"id", 0}, {"n_ctx", n_ctx}, {"is_processing", processing}}});
}

bool parse_tokenize_request(const json & body, TokenizeRequest & out, std::string & err) {
    if (!body.is_object()) {
        err = "the request body must be a JSON object";
        return false;
    }
    if (body.contains("content") && !body["content"].is_null()) {
        if (!body["content"].is_string()) {
            err = "content must be a string (a mixed array of text and token ids is not read here)";
            return false;
        }
        out.content = body["content"].get<std::string>();
    }
    for (auto [key, field] :
         {std::pair{"add_special", &out.add_special}, std::pair{"parse_special", &out.parse_special},
          std::pair{"with_pieces", &out.with_pieces}}) {
        if (!body.contains(key) || body[key].is_null()) continue;
        if (!body[key].is_boolean()) {
            err = std::string(key) + " must be a boolean";
            return false;
        }
        *field = body[key].get<bool>();
    }
    return true;
}

bool parse_detokenize_request(const json & body, std::vector<int32_t> & tokens, std::string & err) {
    if (!body.is_object()) {
        err = "the request body must be a JSON object";
        return false;
    }
    tokens.clear();
    if (!body.contains("tokens") || body["tokens"].is_null()) return true;
    if (!body["tokens"].is_array()) {
        err = "tokens must be an array of token ids";
        return false;
    }
    for (const json & t : body["tokens"]) {
        if (!t.is_number_integer() || t.get<long long>() < 0 || t.get<long long>() > INT32_MAX) {
            err = "tokens must be an array of token ids";
            return false;
        }
        tokens.push_back((int32_t) t.get<long long>());
    }
    return true;
}

json token_piece_json(const std::string & piece) {
    if (complete_utf8_prefix_size(piece) == piece.size()) return piece;
    json bytes = json::array();
    for (unsigned char c : piece)
        bytes.push_back((int) c);
    return bytes;
}

} // namespace bmoe::server
