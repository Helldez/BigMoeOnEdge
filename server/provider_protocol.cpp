#include "provider_protocol.h"

namespace bmoe::server {

namespace {
// How much of an error body that is not JSON is quoted back.
constexpr size_t kErrorQuoteBytes = 300;

json parse_or_string(const std::string & s) {
    try {
        json j = json::parse(s);
        if (j.is_object()) return j;
    } catch (const std::exception &) {
    }
    return json::object();
}
} // namespace

json provider_request_json(const ChatRequest & req, const std::string & model, bool stream) {
    json messages = json::array();
    for (const ChatMessage & m : req.messages) {
        json o{{"role", m.role}, {"content", m.content}};
        if (m.role == "assistant" && !m.tool_calls.empty()) {
            json calls = json::array();
            for (const ChatToolCall & c : m.tool_calls)
                calls.push_back(
                    {{"id", c.id}, {"type", "function"}, {"function", {{"name", c.name}, {"arguments", c.arguments}}}});
            o["tool_calls"] = calls;
        }
        if (m.role == "tool") {
            o["tool_call_id"] = m.tool_call_id;
            if (!m.name.empty()) o["name"] = m.name;
        }
        messages.push_back(o);
    }
    json body{{"model", model}, {"messages", messages}, {"stream", stream}};
    if (!req.tools.empty()) {
        json tools = json::array();
        for (const ChatTool & t : req.tools)
            tools.push_back(
                {{"type", "function"},
                 {"function",
                  {{"name", t.name}, {"description", t.description}, {"parameters", parse_or_string(t.parameters)}}}});
        body["tools"] = tools;
    }
    if (req.n_predict > 0) body["max_tokens"] = req.n_predict;
    return body;
}

void ProviderStream::feed(const char * data, size_t n) {
    pending_.append(data, n);
    size_t nl;
    while ((nl = pending_.find('\n')) != std::string::npos) {
        std::string l = pending_.substr(0, nl);
        pending_.erase(0, nl + 1);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        line(l);
    }
}

void ProviderStream::line(const std::string & l) {
    if (l.compare(0, 5, "data:") != 0) return; // comments, event names, keep-alives
    std::string payload = l.substr(5);
    if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
    if (payload == "[DONE]") {
        done_ = true;
        return;
    }
    json j;
    try {
        j = json::parse(payload);
    } catch (const std::exception &) {
        return; // one malformed event is skipped, not the answer
    }
    ++events_;
    chunk(j);
}

void ProviderStream::chunk(const json & j) {
    if (!j.is_object()) return;
    if (j.contains("error")) {
        error_ = provider_error_message(j.dump());
        return;
    }
    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
    const json & c = j["choices"][0];
    if (!c.is_object()) return;
    if (c.contains("finish_reason") && c["finish_reason"].is_string()) finish_ = c["finish_reason"].get<std::string>();
    // A streamed chunk has `delta`; a whole answer has `message`, read the same way.
    const char * key = c.contains("delta") ? "delta" : "message";
    if (!c.contains(key) || !c[key].is_object()) return;
    const json & d = c[key];
    for (const char * rk : {"reasoning_content", "reasoning"})
        if (d.contains(rk) && d[rk].is_string() && !d[rk].get<std::string>().empty()) {
            reasoning_ += d[rk].get<std::string>();
            changed_ = true;
            break;
        }
    if (d.contains("content") && d["content"].is_string() && !d["content"].get<std::string>().empty()) {
        text_ += d["content"].get<std::string>();
        changed_ = true;
    }
    if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
        for (size_t k = 0; k < d["tool_calls"].size(); ++k) {
            const json & tc = d["tool_calls"][k];
            if (!tc.is_object()) continue;
            const size_t idx = tc.contains("index") && tc["index"].is_number_unsigned() ? tc["index"].get<size_t>() : k;
            if (idx > 64) continue;
            if (calls_.size() <= idx) calls_.resize(idx + 1);
            Call & call = calls_[idx];
            if (tc.contains("id") && tc["id"].is_string()) call.id = tc["id"].get<std::string>();
            if (tc.contains("function") && tc["function"].is_object()) {
                const json & f = tc["function"];
                if (f.contains("name") && f["name"].is_string()) call.name += f["name"].get<std::string>();
                if (f.contains("arguments") && f["arguments"].is_string())
                    call.arguments += f["arguments"].get<std::string>();
                else if (f.contains("arguments") && f["arguments"].is_object())
                    call.arguments = f["arguments"].dump();
            }
        }
    }
}

void ProviderStream::finish_body(const std::string & whole) {
    if (!pending_.empty()) {
        line(pending_);
        pending_.clear();
    }
    if (events_ > 0) return;
    try {
        const json j = json::parse(whole);
        if (j.is_object() && j.contains("choices")) {
            chunk(j);
            ++events_;
            return;
        }
    } catch (const std::exception &) {
    }
    if (error_.empty()) error_ = provider_error_message(whole);
}

std::vector<ChatToolCall> ProviderStream::tool_calls() const {
    std::vector<ChatToolCall> out;
    for (const Call & c : calls_)
        if (!c.name.empty()) out.push_back({c.id, c.name, c.arguments.empty() ? "{}" : c.arguments});
    return out;
}

std::string provider_error_message(const std::string & body) {
    try {
        const json j = json::parse(body);
        if (j.is_object()) {
            if (j.contains("error")) {
                const json & e = j["error"];
                if (e.is_string()) return e.get<std::string>();
                if (e.is_object() && e.contains("message") && e["message"].is_string())
                    return e["message"].get<std::string>();
            }
            if (j.contains("message") && j["message"].is_string()) return j["message"].get<std::string>();
        }
    } catch (const std::exception &) {
    }
    std::string s = body.substr(0, kErrorQuoteBytes);
    s.resize(complete_utf8_prefix_size(s));
    return s.empty() ? "the service answered with no body" : s;
}

} // namespace bmoe::server
