#include "openai_chat.h"

#include <algorithm>

namespace bmoe::server {

namespace {

// A field that must be a string when present. Read through here, never with value(k, ""): that
// throws on a number or an object, and a client's wrong type is a 400, not a server fault.
bool string_field(const json & o, const char * k, const std::string & if_absent, std::string & out) {
    if (!o.contains(k) || o[k].is_null()) {
        out = if_absent;
        return true;
    }
    if (!o[k].is_string()) return false;
    out = o[k].get<std::string>();
    return true;
}

// A message's text: a string, an array of {type:"text"} parts, or null (an assistant turn that only
// calls tools has no content). False for anything else - an image part is not something to drop
// silently.
bool message_text(const json & content, std::string & out) {
    out.clear();
    if (content.is_null()) return true;
    if (content.is_string()) {
        out = content.get<std::string>();
        return true;
    }
    if (!content.is_array()) return false;
    for (const json & part : content) {
        std::string type;
        if (!part.is_object() || !string_field(part, "type", "", type) || type != "text" || !part.contains("text") ||
            !part["text"].is_string())
            return false;
        out += part["text"].get<std::string>();
    }
    return true;
}

// JSON a client may send either as an object or already serialized, as the text the engine carries.
std::string json_text(const json & v, const char * if_absent) {
    if (v.is_null()) return if_absent;
    if (v.is_string()) return v.get<std::string>();
    return v.dump();
}

bool parse_message(const json & m, ChatMessage & out, std::string & err) {
    if (!m.is_object()) {
        err = "every message must be an object";
        return false;
    }
    if (!string_field(m, "role", "", out.role)) {
        err = "a message role must be a string";
        return false;
    }
    if (out.role == "developer") out.role = "system"; // the newer OpenAI name for the same thing
    if (out.role != "system" && out.role != "user" && out.role != "assistant" && out.role != "tool") {
        err = "unsupported message role '" + out.role + "' (system, user, assistant and tool are)";
        return false;
    }
    if (!message_text(m.value("content", json()), out.content)) {
        err = "message content must be a string or an array of text parts";
        return false;
    }
    if (m.contains("reasoning_content") && m["reasoning_content"].is_string())
        out.reasoning = m["reasoning_content"].get<std::string>();
    if (out.role == "tool") {
        if (m.contains("tool_call_id") && m["tool_call_id"].is_string())
            out.tool_call_id = m["tool_call_id"].get<std::string>();
        if (m.contains("name") && m["name"].is_string()) out.name = m["name"].get<std::string>();
    }
    if (out.role == "assistant" && m.contains("tool_calls") && !m["tool_calls"].is_null()) {
        if (!m["tool_calls"].is_array()) {
            err = "tool_calls must be an array";
            return false;
        }
        for (const json & c : m["tool_calls"]) {
            const json fn = c.is_object() ? c.value("function", json()) : json();
            if (!fn.is_object() || !fn.contains("name") || !fn["name"].is_string()) {
                err = "every tool call needs a function with a name";
                return false;
            }
            ChatToolCall call;
            call.id = c.contains("id") && c["id"].is_string() ? c["id"].get<std::string>() : "";
            call.name = fn["name"].get<std::string>();
            call.arguments = json_text(fn.value("arguments", json()), "{}");
            out.tool_calls.push_back(std::move(call));
        }
    }
    return true;
}

bool parse_tools(const json & body, ChatRequest & turn, std::string & err) {
    if (!body.contains("tools") || body["tools"].is_null()) return true;
    if (!body["tools"].is_array()) {
        err = "tools must be an array";
        return false;
    }
    for (const json & t : body["tools"]) {
        const json fn = t.is_object() ? t.value("function", json()) : json();
        std::string type;
        if (!t.is_object() || !string_field(t, "type", "function", type) || type != "function" || !fn.is_object() ||
            !fn.contains("name") || !fn["name"].is_string()) {
            err = "every tool must be {type: \"function\", function: {name, ...}}";
            return false;
        }
        ChatTool tool;
        tool.name = fn["name"].get<std::string>();
        if (fn.contains("description") && fn["description"].is_string())
            tool.description = fn["description"].get<std::string>();
        tool.parameters = json_text(fn.value("parameters", json()), "{\"type\":\"object\",\"properties\":{}}");
        turn.tools.push_back(std::move(tool));
    }
    return true;
}

bool parse_tool_choice(const json & body, ChatRequest & turn, std::string & err) {
    if (!body.contains("tool_choice") || body["tool_choice"].is_null()) return true;
    const json & c = body["tool_choice"];
    if (c.is_object()) {
        // A named function. The engine can require a call but not which one, so this is honoured
        // as "must call" - exact when one tool is offered, which is how clients use it.
        turn.tool_choice = ToolChoice::Required;
        return true;
    }
    const std::string s = c.is_string() ? c.get<std::string>() : "";
    if (s == "auto")
        turn.tool_choice = ToolChoice::Auto;
    else if (s == "none")
        turn.tool_choice = ToolChoice::None;
    else if (s == "required")
        turn.tool_choice = ToolChoice::Required;
    else {
        err = "tool_choice must be \"auto\", \"none\", \"required\" or a function";
        return false;
    }
    return true;
}

} // namespace

bool parse_chat_request(const json & body, const RunConfig & cfg, OpenAiChatRequest & out, std::string & err) {
    if (!body.is_object()) {
        err = "the request body must be a JSON object";
        return false;
    }
    const json msgs = body.value("messages", json());
    if (!msgs.is_array() || msgs.empty()) {
        err = "messages must be a non-empty array";
        return false;
    }
    ChatRequest & turn = out.turn;
    for (const json & m : msgs) {
        ChatMessage msg;
        if (!parse_message(m, msg, err)) return false;
        turn.messages.push_back(std::move(msg));
    }
    // The turn answers its last message. An assistant message there would be a continuation of a
    // half-written answer, which the engine does not do.
    const std::string & last = turn.messages.back().role;
    if (last != "user" && last != "tool") {
        err = "the last message must be from the user or a tool: there is nothing to answer otherwise";
        return false;
    }
    if (!parse_tools(body, turn, err) || !parse_tool_choice(body, turn, err)) return false;

    // A request may ask for less than the configured budget or more, but never for more than the
    // parameter table lets a person set: the session adds this number to the prompt's length, and
    // a value read straight into an int could wrap there. Null and 0 mean "not given".
    turn.n_predict = cfg.n_predict;
    const ParamDesc * budget = find_param("n-predict");
    for (const char * k : {"max_completion_tokens", "max_tokens"}) {
        if (!body.contains(k) || body[k].is_null()) continue;
        const json & v = body[k];
        if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<long long>() < 0)) {
            err = std::string(k) + " must be a non-negative integer";
            return false;
        }
        // Unsigned covers every value that got this far, including one past a signed 64-bit.
        const unsigned long long n = v.get<unsigned long long>();
        if (n == 0) continue;
        const unsigned long long top = (unsigned long long) (budget && budget->bounded ? budget->max : cfg.n_predict);
        turn.n_predict = (int) std::min(n, top);
        break;
    }
    // Thinking, most specific first: this server's own field, the template argument llama.cpp
    // clients send, then the protocol's effort level, of which only "none" is a clear off.
    turn.think = cfg.think;
    if (body.contains("think") && body["think"].is_boolean()) {
        turn.think = body["think"].get<bool>();
    } else if (body.contains("chat_template_kwargs") && body["chat_template_kwargs"].is_object() &&
               body["chat_template_kwargs"].contains("enable_thinking") &&
               body["chat_template_kwargs"]["enable_thinking"].is_boolean()) {
        turn.think = body["chat_template_kwargs"]["enable_thinking"].get<bool>();
    } else if (body.contains("reasoning_effort") && body["reasoning_effort"].is_string() &&
               body["reasoning_effort"].get<std::string>() == "none") {
        turn.think = false;
    }

    out.stream = body.contains("stream") && body["stream"].is_boolean() && body["stream"].get<bool>();
    out.include_usage = body.contains("stream_options") && body["stream_options"].is_object() &&
                        body["stream_options"].value("include_usage", json(false)) == json(true);
    return true;
}

void ensure_tool_call_ids(std::vector<ChatToolCall> & calls, const std::string & response_id) {
    for (size_t i = 0; i < calls.size(); ++i)
        if (calls[i].id.empty()) calls[i].id = "call_" + response_id + "_" + std::to_string(i);
}

std::string finish_reason(const RunResult & r, int n_predict) {
    if (r.cancelled) return "cancelled";
    if (!r.tool_calls.empty()) return "tool_calls";
    // The session may have cut the budget to the room the context had left; that is a length stop too.
    const int budget = r.summary.n_predict > 0 ? std::min(n_predict, r.summary.n_predict) : n_predict;
    return r.summary.n_generated >= budget ? "length" : "stop";
}

json usage_json(const RunSummary & s) {
    // n_past is the context after the turn and n_prompt what was prefilled for it, so the prompt
    // the answer saw is everything before the generated tokens, and the rest of it was reused.
    const int prompt = s.n_past - s.n_generated > s.n_prompt ? s.n_past - s.n_generated : s.n_prompt;
    return {{"prompt_tokens", prompt},
            {"completion_tokens", s.n_generated},
            {"total_tokens", prompt + s.n_generated},
            {"prompt_tokens_details", {{"cached_tokens", prompt - s.n_prompt}}}};
}

namespace {

json tool_calls_json(const std::vector<ChatToolCall> & calls, bool indexed) {
    json out = json::array();
    for (size_t i = 0; i < calls.size(); ++i) {
        json c = json::object();
        if (indexed) c["index"] = (int) i;
        c["id"] = calls[i].id;
        c["type"] = "function";
        c["function"] = {{"name", calls[i].name}, {"arguments", calls[i].arguments}};
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace

json completion_json(const ChatResponseMeta & meta, const RunResult & r, const std::string & finish) {
    json msg = {{"role", "assistant"}};
    // A turn that only calls tools has no text, and the protocol says so with null, not "".
    msg["content"] = r.generated_text.empty() && !r.tool_calls.empty() ? json(nullptr) : json(r.generated_text);
    if (!r.reasoning_text.empty()) msg["reasoning_content"] = r.reasoning_text;
    if (!r.tool_calls.empty()) msg["tool_calls"] = tool_calls_json(r.tool_calls, /*indexed*/ false);
    return {{"id", meta.id},
            {"object", "chat.completion"},
            {"created", meta.created},
            {"model", meta.model},
            {"choices", json::array({{{"index", 0}, {"message", msg}, {"finish_reason", finish}}})},
            {"usage", usage_json(r.summary)},
            {"bmoe", {{"summary", summary_json(r.summary, r.cancelled)}}}};
}

json chunk_json(const ChatResponseMeta & meta, const json & delta, const json & finish) {
    return {{"id", meta.id},
            {"object", "chat.completion.chunk"},
            {"created", meta.created},
            {"model", meta.model},
            {"choices", json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}};
}

json tool_calls_delta(const std::vector<ChatToolCall> & calls) {
    return {{"tool_calls", tool_calls_json(calls, /*indexed*/ true)}};
}

json usage_chunk_json(const ChatResponseMeta & meta, const RunSummary & s) {
    return {{"id", meta.id},       {"object", "chat.completion.chunk"}, {"created", meta.created},
            {"model", meta.model}, {"choices", json::array()},          {"usage", usage_json(s)}};
}

} // namespace bmoe::server
