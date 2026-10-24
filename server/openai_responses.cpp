#include "openai_responses.h"

namespace bmoe::server {

namespace {

const std::string * string_at(const json & o, const char * k) {
    if (!o.is_object() || !o.contains(k) || !o[k].is_string()) return nullptr;
    return o[k].get_ptr<const std::string *>();
}

std::string type_of(const json & o) {
    const std::string * t = string_at(o, "type");
    return t ? *t : "";
}

// A message's or a tool output's content: a string, or text parts (input_text, output_text, and a
// refusal, which is text the model wrote). An image or a file is refused, not dropped.
bool content_text(const json & content, std::string & out, std::string & err) {
    out.clear();
    if (content.is_null()) return true;
    if (content.is_string()) {
        out = content.get<std::string>();
        return true;
    }
    if (!content.is_array()) {
        err = "content must be a string or an array of parts";
        return false;
    }
    for (const json & part : content) {
        const std::string type = type_of(part);
        const char * field = type == "refusal" ? "refusal" : "text";
        if ((type == "input_text" || type == "output_text" || type == "text" || type == "refusal") &&
            string_at(part, field)) {
            out += *string_at(part, field);
        } else if (type == "input_image" || type == "input_file" || type == "input_audio") {
            err = "this server reads text only: an " + type + " part cannot be answered";
            return false;
        } else {
            err = "unsupported content part type '" + type + "'";
            return false;
        }
    }
    return true;
}

// The assistant message an assistant-side item belongs to. One turn of the model arrives as several
// items (its reasoning, its text, each call); a template renders them as one message.
ChatMessage & assistant_turn(std::vector<ChatMessage> & messages) {
    if (messages.empty() || messages.back().role != "assistant") messages.push_back({"assistant", ""});
    return messages.back();
}

std::string tool_name_for(const std::vector<ChatMessage> & messages, const std::string & id) {
    for (auto m = messages.rbegin(); m != messages.rend(); ++m)
        for (const ChatToolCall & c : m->tool_calls)
            if (c.id == id) return c.name;
    return "";
}

bool parse_item(const json & item, std::vector<ChatMessage> & out, std::string & err) {
    if (!item.is_object()) {
        err = "every input item must be an object";
        return false;
    }
    const std::string type = type_of(item);
    if (type.empty() || type == "message") {
        const std::string * role = string_at(item, "role");
        std::string text;
        if (!role || (*role != "user" && *role != "system" && *role != "developer" && *role != "assistant")) {
            err = "a message needs a role: user, system, developer or assistant";
            return false;
        }
        if (!content_text(item.value("content", json()), text, err)) return false;
        if (*role == "assistant") {
            ChatMessage & m = assistant_turn(out);
            m.content += text;
        } else {
            out.push_back({*role == "developer" ? "system" : *role, text});
        }
        return true;
    }
    if (type == "function_call") {
        const std::string * call_id = string_at(item, "call_id");
        const std::string * name = string_at(item, "name");
        if (!call_id || !name) {
            err = "a function_call needs a call_id and a name";
            return false;
        }
        const json args = item.value("arguments", json());
        assistant_turn(out).tool_calls.push_back({*call_id, *name,
                                                  args.is_string() ? args.get<std::string>()
                                                  : args.is_null() ? "{}"
                                                                   : args.dump()});
        return true;
    }
    if (type == "function_call_output") {
        const std::string * call_id = string_at(item, "call_id");
        if (!call_id) {
            err = "a function_call_output needs a call_id";
            return false;
        }
        ChatMessage tool;
        tool.role = "tool";
        tool.tool_call_id = *call_id;
        tool.name = tool_name_for(out, *call_id);
        if (!content_text(item.value("output", json()), tool.content, err)) return false;
        out.push_back(std::move(tool));
        return true;
    }
    if (type == "reasoning") {
        // The text of a reasoning item this server sent; an encrypted one (another server's) and a
        // summary are not the reasoning itself, and a template has nothing to render of them.
        const json content = item.value("content", json());
        if (content.is_array()) {
            std::string text;
            for (const json & part : content)
                if (string_at(part, "text")) text += *string_at(part, "text");
            if (!text.empty()) assistant_turn(out).reasoning += text;
        }
        return true;
    }
    if (type == "item_reference") {
        err = "item_reference names a stored item, and this server stores none: send the item itself";
        return false;
    }
    err = "unsupported input item type '" + type + "'";
    return false;
}

bool parse_tools(const json & body, ChatRequest & turn, std::string & err) {
    if (!body.contains("tools") || body["tools"].is_null()) return true;
    if (!body["tools"].is_array()) {
        err = "tools must be an array";
        return false;
    }
    for (const json & t : body["tools"]) {
        if (type_of(t) != "function") continue; // a built-in tool: see parse_responses_request
        const std::string * name = string_at(t, "name");
        if (!name) {
            err = "every function tool needs a name";
            return false;
        }
        ChatTool tool;
        tool.name = *name;
        if (string_at(t, "description")) tool.description = *string_at(t, "description");
        const json params = t.value("parameters", json());
        tool.parameters = params.is_object() ? params.dump() : "{\"type\":\"object\",\"properties\":{}}";
        turn.tools.push_back(std::move(tool));
    }
    return true;
}

bool parse_tool_choice(const json & body, ChatRequest & turn, std::string & err) {
    if (!body.contains("tool_choice") || body["tool_choice"].is_null()) return true;
    const json & c = body["tool_choice"];
    const std::string s = c.is_string() ? c.get<std::string>() : "";
    if (s == "auto")
        turn.tool_choice = ToolChoice::Auto;
    else if (s == "none")
        turn.tool_choice = ToolChoice::None;
    else if (s == "required" || type_of(c) == "function")
        // A named function: the engine can require a call but not which one, as on the chat endpoint.
        turn.tool_choice = ToolChoice::Required;
    else {
        err = "tool_choice must be \"auto\", \"none\", \"required\" or a function";
        return false;
    }
    return true;
}

// Structured output: text.format, flat where the chat endpoint nests it.
bool parse_text_format(const json & body, ChatRequest & turn, std::string & err) {
    const json text = body.value("text", json());
    if (!text.is_object() || !text.contains("format") || text["format"].is_null()) return true;
    const json & format = text["format"];
    const std::string type = type_of(format);
    if (type == "json_schema" && format.contains("schema") && format["schema"].is_object()) {
        turn.json_schema = format["schema"].dump();
    } else if (type == "json_object") {
        turn.json_schema = R"({"type":"object"})";
    } else if (type != "text") {
        err = "text.format must be {type: \"text\" | \"json_object\" | \"json_schema\", schema}";
        return false;
    }
    return true;
}

json usage(const RunSummary & s) {
    const json u = usage_json(s);
    return {{"input_tokens", u["prompt_tokens"]},
            {"input_tokens_details", {{"cached_tokens", u["prompt_tokens_details"]["cached_tokens"]}}},
            {"output_tokens", u["completion_tokens"]},
            {"total_tokens", u["total_tokens"]}};
}

json reasoning_item(const std::string & id, const std::string & text, const char * status) {
    json content = json::array();
    if (!text.empty()) content.push_back({{"type", "reasoning_text"}, {"text", text}});
    return {{"id", id}, {"type", "reasoning"}, {"status", status}, {"summary", json::array()}, {"content", content}};
}

json output_text_part(const std::string & text) {
    return {{"type", "output_text"}, {"text", text}, {"annotations", json::array()}, {"logprobs", json::array()}};
}

json message_item(const std::string & id, const std::string & text, const char * status) {
    json content = json::array();
    if (std::string(status) == "completed") content.push_back(output_text_part(text));
    return {{"id", id}, {"type", "message"}, {"role", "assistant"}, {"status", status}, {"content", content}};
}

json function_call_item(const std::string & id,
                        const ChatToolCall & c,
                        const std::string & arguments,
                        const char * status) {
    return {{"id", id},        {"type", "function_call"}, {"status", status},
            {"call_id", c.id}, {"name", c.name},          {"arguments", arguments}};
}

std::string item_id(const char * prefix, const ChatResponseMeta & meta, size_t index) {
    return std::string(prefix) + "_" + meta.id + "_" + std::to_string(index);
}

// completed, or incomplete with why: the protocol's statuses for a turn that ended on its budget,
// or was cancelled.
void set_status(json & response, const std::string & finish) {
    if (finish == "length" || finish == "cancelled") {
        response["status"] = "incomplete";
        response["incomplete_details"] = {{"reason", finish == "length" ? "max_output_tokens" : "cancelled"}};
    } else {
        response["status"] = "completed";
        response["incomplete_details"] = nullptr;
    }
}

json response_object(const ChatResponseMeta & meta, const char * status, const json & output) {
    return {{"id", meta.id},    {"object", "response"},         {"created_at", meta.created},
            {"status", status}, {"model", meta.model},          {"output", output},
            {"error", nullptr}, {"incomplete_details", nullptr}};
}

} // namespace

bool parse_responses_request(const json & body, const RunConfig & cfg, ResponsesRequest & out, std::string & err) {
    if (!body.is_object()) {
        err = "the request body must be a JSON object";
        return false;
    }
    const json prev = body.value("previous_response_id", json());
    if (!prev.is_null() && !(prev.is_string() && prev.get<std::string>().empty())) {
        err = "previous_response_id is not supported: this server keeps no responses, so send the whole "
              "conversation in input (and store: false)";
        return false;
    }
    if (body.contains("conversation") && !body["conversation"].is_null()) {
        err = "conversation is not supported: this server keeps no conversations; send them in input";
        return false;
    }
    if (body.contains("background") && body["background"] == json(true)) {
        err = "background responses are not supported: a response is answered on its own request";
        return false;
    }
    ChatRequest & turn = out.turn;
    if (body.contains("instructions") && !body["instructions"].is_null()) {
        if (!body["instructions"].is_string()) {
            err = "instructions must be a string";
            return false;
        }
        turn.messages.push_back({"system", body["instructions"].get<std::string>()});
    }
    const json input = body.value("input", json());
    if (input.is_string()) {
        turn.messages.push_back({"user", input.get<std::string>()});
    } else if (input.is_array()) {
        for (const json & item : input)
            if (!parse_item(item, turn.messages, err)) return false;
    } else {
        err = "input must be a string or an array of items";
        return false;
    }
    place_system_messages(turn.messages);
    if (turn.messages.empty() || (turn.messages.back().role != "user" && turn.messages.back().role != "tool")) {
        err = "the last input item must be from the user or a function_call_output: there is nothing to answer "
              "otherwise";
        return false;
    }
    if (!parse_tools(body, turn, err) || !parse_tool_choice(body, turn, err) || !parse_text_format(body, turn, err))
        return false;

    bool given = false;
    turn.n_predict = cfg.n_predict;
    if (!read_token_budget(body, "max_output_tokens", cfg, given, turn.n_predict, err)) return false;
    // reasoning.effort: "none" is off, any other level asks for reasoning; absent, the configured one.
    turn.think = cfg.think;
    const json reasoning = body.value("reasoning", json());
    if (reasoning.is_object() && string_at(reasoning, "effort")) turn.think = *string_at(reasoning, "effort") != "none";
    // store, include, parallel_tool_calls, prompt_cache_key, temperature and top_p are read by
    // nothing: nothing is stored, there is no encrypted reasoning to include, and the engine samples
    // with its own settings.
    out.stream = body.contains("stream") && body["stream"] == json(true);
    return true;
}

json responses_error_json(int status, const std::string & message, const std::string & code) {
    const bool full = code == run_error_name(RunError::ContextFull);
    return {{"error",
             {{"message", message},
              {"type", status >= 500 ? "server_error" : "invalid_request_error"},
              {"param", nullptr},
              {"code", full           ? json("context_length_exceeded")
                       : code.empty() ? json(nullptr)
                                      : json(code)}}}};
}

json response_json(const ChatResponseMeta & meta, const RunResult & r, const std::string & finish) {
    json output = json::array();
    if (!r.reasoning_text.empty())
        output.push_back(reasoning_item(item_id("rs", meta, output.size()), r.reasoning_text, "completed"));
    if (!r.generated_text.empty())
        output.push_back(message_item(item_id("msg", meta, output.size()), r.generated_text, "completed"));
    for (const ChatToolCall & c : r.tool_calls)
        output.push_back(function_call_item(item_id("fc", meta, output.size()), c, c.arguments, "completed"));
    json res = response_object(meta, "completed", output);
    set_status(res, finish);
    res["usage"] = usage(r.summary);
    res["bmoe"] = {{"summary", summary_json(r.summary, r.cancelled)}};
    return res;
}

std::string ResponsesStream::event(const std::string & type, json data) {
    json e = {{"type", type}, {"sequence_number", seq_++}};
    e.update(data);
    return sse_event(type, e);
}

std::string ResponsesStream::start() {
    output_ = json::array();
    const json r = response_object(meta_, "in_progress", json::array());
    return event("response.created", {{"response", r}}) + event("response.in_progress", {{"response", r}});
}

std::string ResponsesStream::frames(const std::vector<BlockEvent> & events) {
    std::string f;
    for (const BlockEvent & e : events) {
        const bool reasoning = e.kind == BlockKind::Reasoning;
        const int index = (int) output_.size();
        switch (e.type) {
        case BlockEvent::Type::Open:
            open_.clear();
            open_id_ = item_id(reasoning ? "rs" : "msg", meta_, output_.size());
            f += event("response.output_item.added", {{"output_index", index},
                                                      {"item", reasoning ? reasoning_item(open_id_, "", "in_progress")
                                                                         : message_item(open_id_, "", "in_progress")}});
            if (!reasoning)
                f += event("response.content_part.added", {{"item_id", open_id_},
                                                           {"output_index", index},
                                                           {"content_index", 0},
                                                           {"part", output_text_part("")}});
            break;
        case BlockEvent::Type::Delta:
            open_ += e.text;
            f += event(reasoning ? "response.reasoning_text.delta" : "response.output_text.delta",
                       {{"item_id", open_id_}, {"output_index", index}, {"content_index", 0}, {"delta", e.text}});
            break;
        case BlockEvent::Type::Close: {
            json item;
            if (reasoning) {
                f += event("response.reasoning_text.done",
                           {{"item_id", open_id_}, {"output_index", index}, {"content_index", 0}, {"text", open_}});
                item = reasoning_item(open_id_, open_, "completed");
            } else {
                f += event("response.output_text.done",
                           {{"item_id", open_id_}, {"output_index", index}, {"content_index", 0}, {"text", open_}});
                f += event("response.content_part.done", {{"item_id", open_id_},
                                                          {"output_index", index},
                                                          {"content_index", 0},
                                                          {"part", output_text_part(open_)}});
                item = message_item(open_id_, open_, "completed");
            }
            f += event("response.output_item.done", {{"output_index", index}, {"item", item}});
            output_.push_back(std::move(item));
            break;
        }
        }
    }
    return f;
}

std::string ResponsesStream::on_token(const std::string & text, const std::string & reasoning) {
    return frames(blocks_.update(text, reasoning));
}

std::string ResponsesStream::finish(const RunResult & r, const std::string & finish) {
    std::string f = frames(blocks_.finish(r.generated_text, r.reasoning_text));
    // A call is known whole once the turn is parsed, so its arguments travel in one delta.
    for (const ChatToolCall & c : r.tool_calls) {
        const int index = (int) output_.size();
        const std::string id = item_id("fc", meta_, output_.size());
        f += event("response.output_item.added",
                   {{"output_index", index}, {"item", function_call_item(id, c, "", "in_progress")}});
        f += event("response.function_call_arguments.delta",
                   {{"item_id", id}, {"output_index", index}, {"delta", c.arguments}});
        f += event("response.function_call_arguments.done",
                   {{"item_id", id}, {"output_index", index}, {"name", c.name}, {"arguments", c.arguments}});
        json item = function_call_item(id, c, c.arguments, "completed");
        f += event("response.output_item.done", {{"output_index", index}, {"item", item}});
        output_.push_back(std::move(item));
    }
    json res = response_object(meta_, "completed", output_);
    set_status(res, finish);
    res["usage"] = usage(r.summary);
    res["bmoe"] = {{"summary", summary_json(r.summary, r.cancelled)}};
    const bool complete = res["status"] == "completed";
    return f + event(complete ? "response.completed" : "response.incomplete", {{"response", res}});
}

std::string ResponsesStream::error(int status, const std::string & message, const std::string & code) {
    json res = response_object(meta_, "failed", output_);
    res["error"] = responses_error_json(status, message, code)["error"];
    res["error"].erase("param");
    res["error"].erase("type");
    return event("response.failed", {{"response", res}});
}

} // namespace bmoe::server
