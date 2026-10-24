#include "anthropic_messages.h"

namespace bmoe::server {

namespace {

// The first system block Claude Code sends: client metadata, not instructions, and different on
// every request (it carries a per-request stamp). Kept, it would change the prompt's first tokens
// each time, and no turn could reuse the context the KV holds.
constexpr const char * kBillingHeader = "x-anthropic-billing-header:";
// How the text blocks of one message, or of the system prompt, are joined into one text.
constexpr const char * kBlockSeparator = "\n\n";

const std::string * string_at(const json & o, const char * k) {
    if (!o.is_object() || !o.contains(k) || !o[k].is_string()) return nullptr;
    return o[k].get_ptr<const std::string *>();
}

std::string type_of(const json & block) {
    const std::string * t = string_at(block, "type");
    return t ? *t : "";
}

void join(std::string & into, const std::string & text) {
    if (text.empty()) return;
    if (!into.empty()) into += kBlockSeparator;
    into += text;
}

bool unsupported_block(const std::string & type, std::string & err) {
    if (type == "image" || type == "document")
        err = "this server reads text only: a message with an " + type + " block cannot be answered";
    else
        err = "unsupported content block type '" + type + "'";
    return false;
}

bool parse_system(const json & body, std::string & out, std::string & err) {
    out.clear();
    if (!body.contains("system") || body["system"].is_null()) return true;
    const json & sys = body["system"];
    if (sys.is_string()) {
        out = sys.get<std::string>();
        return true;
    }
    if (!sys.is_array()) {
        err = "system must be a string or an array of text blocks";
        return false;
    }
    for (const json & b : sys) {
        const std::string * text = string_at(b, "text");
        if (type_of(b) != "text" || !text) {
            err = "every system block must be {type: \"text\", text}";
            return false;
        }
        std::string t = *text;
        if (t.rfind(kBillingHeader, 0) == 0) {
            const size_t nl = t.find('\n');
            t = nl == std::string::npos ? "" : t.substr(nl + 1);
        }
        join(out, t);
    }
    return true;
}

// A tool_result's content: a string, or blocks of text. tool_reference is Claude Code's tool search
// naming a tool it has just made available; the API expands it into the tool's definition, which
// this server cannot, so the model reads that the tool was loaded (its schema comes in `tools`).
bool tool_result_text(const json & content, std::string & out, std::string & err) {
    out.clear();
    if (content.is_null()) return true;
    if (content.is_string()) {
        out = content.get<std::string>();
        return true;
    }
    if (!content.is_array()) {
        err = "a tool_result's content must be a string or an array of blocks";
        return false;
    }
    for (const json & b : content) {
        const std::string type = type_of(b);
        if (type == "text" && string_at(b, "text")) {
            join(out, *string_at(b, "text"));
        } else if (type == "tool_reference" && string_at(b, "tool_name")) {
            join(out, "Tool loaded: " + *string_at(b, "tool_name"));
        } else {
            return unsupported_block(type, err);
        }
    }
    return true;
}

std::string tool_name_for(const std::vector<ChatMessage> & messages, const std::string & id) {
    for (auto m = messages.rbegin(); m != messages.rend(); ++m)
        for (const ChatToolCall & c : m->tool_calls)
            if (c.id == id) return c.name;
    return "";
}

// One protocol message, as the engine's messages: a user message's tool results become tool
// messages (first, as the protocol orders them), its text one user message after them.
bool parse_message(const json & m, std::vector<ChatMessage> & out, std::string & err) {
    const std::string * role = string_at(m, "role");
    if (!role || (*role != "user" && *role != "assistant" && *role != "system")) {
        err = "every message needs a role: user, assistant or system";
        return false;
    }
    ChatMessage msg;
    msg.role = *role;
    const json content = m.is_object() ? m.value("content", json()) : json();
    if (content.is_string()) {
        msg.content = content.get<std::string>();
        out.push_back(std::move(msg));
        return true;
    }
    if (!content.is_array()) {
        err = "message content must be a string or an array of blocks";
        return false;
    }
    for (const json & b : content) {
        const std::string type = type_of(b);
        if (type == "text" && string_at(b, "text")) {
            join(msg.content, *string_at(b, "text"));
        } else if (msg.role == "assistant" && type == "thinking" && string_at(b, "thinking")) {
            msg.reasoning += *string_at(b, "thinking");
        } else if (msg.role == "assistant" && type == "redacted_thinking") {
            continue; // encrypted by another server: nothing a template can render
        } else if (msg.role == "assistant" && type == "tool_use") {
            const std::string * id = string_at(b, "id");
            const std::string * name = string_at(b, "name");
            const json input = b.value("input", json::object());
            if (!id || !name || !input.is_object()) {
                err = "a tool_use block needs an id, a name and an input object";
                return false;
            }
            msg.tool_calls.push_back({*id, *name, input.dump()});
        } else if (msg.role == "user" && type == "tool_result") {
            const std::string * id = string_at(b, "tool_use_id");
            if (!id) {
                err = "a tool_result block needs a tool_use_id";
                return false;
            }
            ChatMessage tool;
            tool.role = "tool";
            tool.tool_call_id = *id;
            tool.name = tool_name_for(out, *id);
            if (!tool_result_text(b.value("content", json()), tool.content, err)) return false;
            out.push_back(std::move(tool));
        } else {
            return unsupported_block(type, err);
        }
    }
    // A user message that only answered tools has nothing left to say.
    if (msg.role != "user" || !msg.content.empty()) out.push_back(std::move(msg));
    return true;
}

bool parse_tools(const json & body, ChatRequest & turn, std::string & err) {
    if (!body.contains("tools") || body["tools"].is_null()) return true;
    if (!body["tools"].is_array()) {
        err = "tools must be an array";
        return false;
    }
    for (const json & t : body["tools"]) {
        const std::string type = type_of(t);
        const std::string * name = string_at(t, "name");
        // A server tool (web search, code execution) is run by the API itself, not by the client:
        // there is nobody here to run it, and a model told of it would answer with made-up results.
        if (!type.empty() && type != "custom") {
            err = "server tool '" + type + "' is not supported: only client tools (name, input_schema) are";
            return false;
        }
        if (!name || !t.contains("input_schema") || !t["input_schema"].is_object()) {
            err = "every tool needs a name and an input_schema object";
            return false;
        }
        // A deferred tool is not shown to the model until tool search loads it; the client sends it
        // again, without the flag, once it has been.
        if (t.contains("defer_loading") && t["defer_loading"] == json(true)) continue;
        ChatTool tool;
        tool.name = *name;
        if (string_at(t, "description")) tool.description = *string_at(t, "description");
        tool.parameters = t["input_schema"].dump();
        turn.tools.push_back(std::move(tool));
    }
    return true;
}

bool parse_tool_choice(const json & body, ChatRequest & turn, std::string & err) {
    if (!body.contains("tool_choice") || body["tool_choice"].is_null()) return true;
    const std::string type = type_of(body["tool_choice"]);
    if (type == "auto")
        turn.tool_choice = ToolChoice::Auto;
    else if (type == "none")
        turn.tool_choice = ToolChoice::None;
    else if (type == "any" || type == "tool")
        // A named tool: the engine can require a call but not which one, as on the chat endpoint.
        turn.tool_choice = ToolChoice::Required;
    else {
        err = "tool_choice must be {type: \"auto\" | \"any\" | \"tool\" | \"none\"}";
        return false;
    }
    return true;
}

bool parse_thinking(const json & body, ChatRequest & turn, std::string & err) {
    if (!body.contains("thinking") || body["thinking"].is_null()) return true;
    const std::string type = type_of(body["thinking"]);
    if (type == "enabled" || type == "adaptive")
        turn.think = true;
    else if (type == "disabled")
        turn.think = false;
    else {
        err = "thinking must be {type: \"enabled\" | \"adaptive\" | \"disabled\"}";
        return false;
    }
    return true;
}

// Structured output: output_config.format, a JSON schema the answer must match.
bool parse_output_format(const json & body, ChatRequest & turn, std::string & err) {
    const json cfg = body.value("output_config", json());
    if (!cfg.is_object() || !cfg.contains("format") || cfg["format"].is_null()) return true;
    const json & format = cfg["format"];
    if (type_of(format) != "json_schema" || !format.contains("schema") || !format["schema"].is_object()) {
        err = "output_config.format must be {type: \"json_schema\", schema: {...}}";
        return false;
    }
    turn.json_schema = format["schema"].dump();
    return true;
}

const char * error_type(int status) {
    switch (status) {
    case 400:
    case 415:
        return "invalid_request_error";
    case 401:
        return "authentication_error";
    case 403:
        return "permission_error";
    case 404:
        return "not_found_error";
    case 413:
        return "request_too_large";
    case 429:
        return "rate_limit_error";
    case 503:
        // No model loaded, or one still loading: the protocol's word for "try again shortly", which
        // a client retries with backoff, as it should while a load finishes.
        return "overloaded_error";
    default:
        return "api_error";
    }
}

json content_block_start(int index, const json & block) {
    return {{"type", "content_block_start"}, {"index", index}, {"content_block", block}};
}

json content_block_delta(int index, const json & delta) {
    return {{"type", "content_block_delta"}, {"index", index}, {"delta", delta}};
}

json content_block_stop(int index) {
    return {{"type", "content_block_stop"}, {"index", index}};
}

// A call's arguments are the JSON object the model wrote. One that is not an object cannot be a
// tool's input, and a client that parses it would fail on the whole message.
json tool_input(const std::string & arguments) {
    try {
        json v = json::parse(arguments);
        if (v.is_object()) return v;
    } catch (const std::exception &) {
    }
    return json::object();
}

} // namespace

bool parse_messages_request(const json & body, const RunConfig & cfg, AnthropicRequest & out, std::string & err) {
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
    std::string system;
    if (!parse_system(body, system, err)) return false;
    if (!system.empty()) turn.messages.push_back({"system", system});
    for (const json & m : msgs)
        if (!parse_message(m, turn.messages, err)) return false;
    place_system_messages(turn.messages);
    if (turn.messages.empty() || (turn.messages.back().role != "user" && turn.messages.back().role != "tool")) {
        err = "the last message must be from the user or answer a tool: the engine does not continue a "
              "prefilled answer";
        return false;
    }
    if (!parse_tools(body, turn, err) || !parse_tool_choice(body, turn, err) || !parse_output_format(body, turn, err))
        return false;

    // max_tokens is required by the protocol; a client that leaves it out gets the configured budget.
    bool given = false;
    turn.n_predict = cfg.n_predict;
    if (!read_token_budget(body, "max_tokens", cfg, given, turn.n_predict, err)) return false;
    turn.think = cfg.think;
    if (!parse_thinking(body, turn, err)) return false;
    // temperature, top_p, top_k, stop_sequences, metadata, cache_control and context_management are
    // read by nothing: the engine samples with its own settings, and has no stop strings.
    out.stream = body.contains("stream") && body["stream"] == json(true);
    return true;
}

json anthropic_error_json(int status, const std::string & message, const std::string & code) {
    std::string type = error_type(status);
    std::string text = message;
    if (code == run_error_name(RunError::ContextFull)) {
        type = "invalid_request_error";
        text = "prompt is too long: " + message;
    }
    return {{"type", "error"}, {"error", {{"type", type}, {"message", text}}}};
}

std::string anthropic_stop_reason(const std::string & finish) {
    if (finish == "length") return "max_tokens";
    if (finish == "tool_calls") return "tool_use";
    return "end_turn"; // stop, and a cancelled turn, which the protocol has no word for
}

json anthropic_usage_json(const RunSummary & s) {
    const json u = usage_json(s);
    const int prompt = u["prompt_tokens"].get<int>();
    return {{"input_tokens", s.n_prompt},
            {"cache_creation_input_tokens", 0},
            {"cache_read_input_tokens", prompt - s.n_prompt},
            {"output_tokens", s.n_generated}};
}

json message_json(const ChatResponseMeta & meta, const RunResult & r, const std::string & finish) {
    json content = json::array();
    // A thinking block carries a signature the API makes and checks; a local model has none to give.
    if (!r.reasoning_text.empty())
        content.push_back({{"type", "thinking"}, {"thinking", r.reasoning_text}, {"signature", ""}});
    if (!r.generated_text.empty()) content.push_back({{"type", "text"}, {"text", r.generated_text}});
    for (const ChatToolCall & c : r.tool_calls)
        content.push_back({{"type", "tool_use"}, {"id", c.id}, {"name", c.name}, {"input", tool_input(c.arguments)}});
    return {{"id", meta.id},
            {"type", "message"},
            {"role", "assistant"},
            {"model", meta.model},
            {"content", content},
            {"stop_reason", anthropic_stop_reason(finish)},
            {"stop_sequence", nullptr},
            {"usage", anthropic_usage_json(r.summary)},
            {"bmoe", {{"summary", summary_json(r.summary, r.cancelled)}}}};
}

std::string AnthropicStream::start() const {
    // The prompt's counts are known only once the turn has run: message_delta carries them.
    const json message = {{"id", meta_.id},           {"type", "message"},
                          {"role", "assistant"},      {"model", meta_.model},
                          {"content", json::array()}, {"stop_reason", nullptr},
                          {"stop_sequence", nullptr}, {"usage", {{"input_tokens", 0}, {"output_tokens", 0}}}};
    return sse_event("message_start", {{"type", "message_start"}, {"message", message}});
}

std::string AnthropicStream::ping() {
    return sse_event("ping", {{"type", "ping"}});
}

std::string AnthropicStream::frames(const std::vector<BlockEvent> & events) {
    std::string f;
    for (const BlockEvent & e : events) {
        const bool thinking = e.kind == BlockKind::Reasoning;
        switch (e.type) {
        case BlockEvent::Type::Open:
            f += sse_event("content_block_start",
                           content_block_start(index_, thinking ? json{{"type", "thinking"}, {"thinking", ""}}
                                                                : json{{"type", "text"}, {"text", ""}}));
            break;
        case BlockEvent::Type::Delta:
            f += sse_event("content_block_delta",
                           content_block_delta(index_, thinking ? json{{"type", "thinking_delta"}, {"thinking", e.text}}
                                                                : json{{"type", "text_delta"}, {"text", e.text}}));
            break;
        case BlockEvent::Type::Close:
            // A thinking block ends with its signature, which a client expects before the stop.
            if (thinking)
                f += sse_event("content_block_delta",
                               content_block_delta(index_, {{"type", "signature_delta"}, {"signature", ""}}));
            f += sse_event("content_block_stop", content_block_stop(index_++));
            break;
        }
    }
    return f;
}

std::string AnthropicStream::on_token(const std::string & text, const std::string & reasoning) {
    return frames(blocks_.update(text, reasoning));
}

std::string AnthropicStream::finish(const RunResult & r, const std::string & finish) {
    std::string f = frames(blocks_.finish(r.generated_text, r.reasoning_text));
    // A call is known whole once the turn is parsed, so its input travels in one delta.
    for (const ChatToolCall & c : r.tool_calls) {
        f += sse_event("content_block_start",
                       content_block_start(
                           index_, {{"type", "tool_use"}, {"id", c.id}, {"name", c.name}, {"input", json::object()}}));
        f += sse_event("content_block_delta",
                       content_block_delta(
                           index_, {{"type", "input_json_delta"}, {"partial_json", tool_input(c.arguments).dump()}}));
        f += sse_event("content_block_stop", content_block_stop(index_++));
    }
    f += sse_event("message_delta",
                   {{"type", "message_delta"},
                    {"delta", {{"stop_reason", anthropic_stop_reason(finish)}, {"stop_sequence", nullptr}}},
                    {"usage", anthropic_usage_json(r.summary)},
                    {"bmoe", {{"summary", summary_json(r.summary, r.cancelled)}}}});
    f += sse_event("message_stop", {{"type", "message_stop"}});
    return f;
}

std::string AnthropicStream::error(int status, const std::string & message, const std::string & code) {
    return sse_event("error", anthropic_error_json(status, message, code));
}

} // namespace bmoe::server
