// Unit tests for bmoe-server's policy (server/): the rules that decide what the engine is fed and
// which value wins, driven without HTTP, a session or a model.
//
//   * the chat protocol: which request fields become an engine turn, and what a turn's result
//     (text, tool calls, token counts) looks like on the wire;
//   * the turn queue: first come first served, a bound, and a caller that gives its place up;
//   * the settings layers: defaults < plan < launch < user, what a reset removes, what persists;
//   * the JSON edges: form values to parameter strings and back;
//   * access: which host names, origins, keys and content types reach the API, and when the server
//     may not start at all;
//   * decide: the request fields a decision is read from, and its answer on the wire;
//   * model files: which names make a split set, and how a directory's files group into models;
//   * the plan view: one shape for every machine (an NPU phone, a discrete card, the cores alone),
//     the device roles and their words, the memory pools, the decision groups.
//   * downloads: which curl failures are retried and after how long, what a resume still needs on
//     disk, and that deleting a partial download stays inside the models folder;
//   * events: how many streams may be open, and that a stream that ends gives its place back;
//   * a model from a link: which links are taken, the entry they become, the size a HEAD gives,
//     and that a server with no curl leaves the size unknown rather than refuse the link;
//   * deleting a model: named as the listing names it, a split set whole, nothing outside the
//     models folder, the loaded model recognised by identity.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "access.h"
#include "agent_protocol.h"
#include "anthropic_messages.h"
#include "decide_protocol.h"
#include "download_policy.h"
#include "event_bus.h"
#include "hf_repo.h"
#include "http_fetch.h"
#include "json_util.h"
#include "model_delete.h"
#include "model_files.h"
#include "openai_chat.h"
#include "openai_responses.h"
#include "plan_view.h"
#include "settings_store.h"
#include "sha256.h"
#include "turn_queue.h"
#include "url_entry.h"

#include "bmoe/planner.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace bmoe;
using namespace bmoe::server;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

static OpenAiChatRequest
parsed(const char * body, bool * ok = nullptr, std::string * err = nullptr, bool think = true) {
    OpenAiChatRequest r;
    std::string e;
    RunConfig cfg;
    cfg.n_predict = 100;
    cfg.think = think;
    const bool good = parse_chat_request(json::parse(body), cfg, r, e);
    if (ok) *ok = good;
    if (err) *err = e;
    return r;
}

static void protocol_tests() {
    bool ok = false;
    std::string err;
    {
        const OpenAiChatRequest r = parsed(R"j({"messages":[{"role":"developer","content":"be brief"},
            {"role":"user","content":[{"type":"text","text":"a"},{"type":"text","text":"b"}]}]})j",
                                           &ok);
        check(ok && r.turn.messages.size() == 2 && r.turn.messages[0].role == "system" &&
                  r.turn.messages[1].content == "ab",
              "developer is system, and text parts join");
        check(r.turn.n_predict == 100 && r.turn.think && !r.stream && !r.include_usage && r.turn.tools.empty() &&
                  r.turn.json_schema.empty(),
              "what a request leaves out comes from the config");
    }
    {
        const OpenAiChatRequest r = parsed(R"j({"messages":[{"role":"user","content":"read it"},
            {"role":"assistant","content":null,"tool_calls":[{"id":"c1","type":"function",
              "function":{"name":"read","arguments":"{\"path\":\"a.py\"}"}}]},
            {"role":"tool","tool_call_id":"c1","content":"print(1)"}],
            "tools":[{"type":"function","function":{"name":"read","description":"Read a file",
              "parameters":{"type":"object","properties":{"path":{"type":"string"}}}}}],
            "tool_choice":"required","stream":true,"stream_options":{"include_usage":true},
            "max_tokens":7})j",
                                           &ok);
        check(ok && r.turn.messages.size() == 3, "an agent's history parses");
        const ChatMessage & call = r.turn.messages[1];
        check(call.content.empty() && call.tool_calls.size() == 1 && call.tool_calls[0].id == "c1" &&
                  call.tool_calls[0].name == "read" && call.tool_calls[0].arguments == "{\"path\":\"a.py\"}",
              "an assistant turn with null content keeps its call");
        check(r.turn.messages[2].role == "tool" && r.turn.messages[2].tool_call_id == "c1" &&
                  r.turn.messages[2].content == "print(1)",
              "a tool message keeps the call it answers");
        check(r.turn.tools.size() == 1 && r.turn.tools[0].name == "read" &&
                  json::parse(r.turn.tools[0].parameters)["properties"].contains("path"),
              "a tool's schema travels as JSON text");
        check(r.turn.tool_choice == ToolChoice::Required && r.stream && r.include_usage && r.turn.n_predict == 7,
              "tool_choice, stream, include_usage and max_tokens are read");
    }
    {
        const OpenAiChatRequest r = parsed(R"j({"messages":[{"role":"user","content":"x"},
            {"role":"assistant","tool_calls":[{"function":{"name":"f","arguments":{"k":1}}}]},
            {"role":"tool","content":"ok"}]})j",
                                           &ok);
        check(ok && r.turn.messages[1].tool_calls[0].arguments == "{\"k\":1}",
              "arguments sent as an object are carried as their text");
    }
    parsed(R"j({"messages":[{"role":"user","content":"hi"},{"role":"assistant","content":"yo"}]})j", &ok, &err);
    check(!ok && err.find("last message") != std::string::npos, "a conversation ending on an answer is refused");
    parsed(R"j({"messages":[{"role":"user","content":[{"type":"image_url","image_url":{"url":"x"}}]}]})j", &ok);
    check(!ok, "an image part is refused, not dropped");
    parsed(R"j({"messages":[{"role":"user","content":"hi"}],"tools":[{"type":"web_search"}]})j", &ok);
    check(!ok, "a tool that is not a function is refused");
    parsed(R"j({"messages":[{"role":"user","content":"hi"}],"tool_choice":"sometimes"})j", &ok);
    check(!ok, "an unknown tool_choice is refused");
    parsed(R"j({"messages":[]})j", &ok);
    check(!ok, "an empty conversation is refused");
    // A field of the wrong type is the client's mistake: refused with a reason, never thrown.
    parsed(R"j({"messages":[{"role":1,"content":"hi"}]})j", &ok, &err);
    check(!ok && err.find("role") != std::string::npos, "a role that is not a string is refused, not thrown");
    parsed(R"j({"messages":[{"role":"user","content":[{"type":{"x":1},"text":"a"}]}]})j", &ok, &err);
    check(!ok && err.find("content") != std::string::npos, "a part type that is not a string is refused");
    parsed(R"j({"messages":[{"role":"user","content":"hi"}],"tools":[{"type":7,"function":{"name":"f"}}]})j", &ok);
    check(!ok, "a tool type that is not a string is refused");
    parsed(R"j({"messages":[{"role":"user","content":"hi"}],"tools":[{"function":{"name":"f"}}]})j", &ok);
    check(ok, "a tool with no type is a function, as before");
    parsed(R"j({"messages":[{"role":null,"content":"hi"}]})j", &ok, &err);
    check(!ok && err.find("role") != std::string::npos, "a null role is an unsupported one");
    int shape = 0;
    for (const char * body : {R"j({"messages":[{"role":"user","content":"hi","tool_calls":7}]})j",
                              R"j({"messages":[{"role":"assistant","content":"a","tool_calls":[{"function":
                                  {"name":"f","arguments":[1]}}]},{"role":"user","content":"b"}]})j",
                              R"j({"messages":"hi"})j", R"j({"messages":[[1]]})j", R"j([1])j", R"j(7)j",
                              R"j({"messages":[{"role":"user","content":"hi"}],"stream":"yes","think":3,
                                  "stream_options":7,"tool_choice":7,"max_tokens":"9"})j"}) {
        bool threw = false;
        try {
            parsed(body, &ok, &err);
        } catch (const std::exception &) {
            threw = true;
        }
        check(!threw, "a body of the wrong shape is answered, not thrown (#" + std::to_string(++shape) + ")");
    }
    check(!parsed(R"j({"messages":[{"role":"user","content":"hi"}],"think":false})j").turn.think &&
              !parsed(R"j({"messages":[{"role":"user","content":"hi"}],"reasoning_effort":"none"})j").turn.think &&
              parsed(R"j({"messages":[{"role":"user","content":"hi"}],"reasoning_effort":"low"})j").turn.think &&
              parsed(R"j({"messages":[{"role":"user","content":"hi"}],"reasoning_effort":"none","think":true})j")
                  .turn.think,
          "thinking: the server's own field wins, and only effort none turns it off");
    check(
        parsed(R"j({"messages":[{"role":"user","content":"hi"}],"reasoning_effort":"high"})j", nullptr, nullptr, false)
                .turn.think &&
            !parsed(R"j({"messages":[{"role":"user","content":"hi"}]})j", nullptr, nullptr, false).turn.think,
        "thinking off by default: any effort but none turns it on, and no field keeps the default");

    const std::pair<const char *, const char *> formats[] = {
        {"null", ""},
        {"{}", ""},
        {R"j({"type":null})j", ""},
        {R"j({"type":""})j", ""},
        {R"j({"type":"text"})j", ""},
        {R"j({"type":"json_object"})j", R"j({"type":"object"})j"},
        {R"j({"type":"json_object","schema":{}})j", R"j({"type":"object"})j"},
        {R"j({"type":"json_object","schema":null})j", R"j({"type":"object"})j"},
        {R"j({"type":"json_schema"})j", R"j({"type":"object"})j"},
        {R"j({"type":"json_schema","json_schema":null})j", R"j({"type":"object"})j"},
        {R"j({"type":"json_schema","json_schema":{"schema":{}}})j", R"j({"type":"object"})j"},
        {R"j({"type":"json_schema","json_schema":{"name":"answer","description":"An answer","strict":true,
              "schema":{"type":"array","items":{"type":"string"}}}})j",
         R"j({"type":"array","items":{"type":"string"}})j"},
        {R"j({"type":"json_object","schema":{"type":"object","properties":{"city":{"type":"string"}},
              "required":["city"],"additionalProperties":false}})j",
         R"j({"type":"object","properties":{"city":{"type":"string"}},"required":["city"],"additionalProperties":false})j"},
    };
    for (const auto & format : formats) {
        const std::string body =
            std::string(R"j({"messages":[{"role":"user","content":"hi"}],"response_format":)j") + format.first + "}";
        const OpenAiChatRequest r = parsed(body.c_str(), &ok, &err);
        check(ok && r.turn.json_schema == format.second,
              "response_format maps to the template schema: " + std::string(format.first));
    }
    const std::pair<const char *, const char *> schema_fields[] = {
        {R"j("json_schema":null)j", ""},
        {R"j("json_schema":{})j", R"j({"type":"object"})j"},
        {R"j("json_schema":{"type":"array","items":{"type":"integer"}})j",
         R"j({"type":"array","items":{"type":"integer"}})j"},
        {R"j("json_schema":{"type":"string"},"response_format":{"type":"text"})j", R"j({"type":"string"})j"},
        {R"j("json_schema":{"type":"string"},"response_format":{"type":"json_object"})j", R"j({"type":"string"})j"},
        {R"j("json_schema":{"type":"string"},"response_format":{"type":"json_object","schema":{}})j",
         R"j({"type":"object"})j"},
        {R"j("json_schema":{"type":"string"},"response_format":{"type":"json_schema"})j", R"j({"type":"object"})j"},
        {R"j("json_schema":{"type":"string"},"response_format":{"type":"json_schema",
              "json_schema":{"schema":{"type":"integer"}}})j",
         R"j({"type":"integer"})j"},
    };
    for (const auto & fields : schema_fields) {
        const std::string body = std::string(R"j({"messages":[{"role":"user","content":"hi"}],)j") + fields.first + "}";
        const OpenAiChatRequest r = parsed(body.c_str(), &ok, &err);
        check(ok && r.turn.json_schema == fields.second,
              "top-level schema and response_format keep upstream precedence: " + std::string(fields.first));
    }
    for (const bool thinking : {false, true}) {
        json body = {{"messages", {{{"role", "user"}, {"content", "hi"}}}},
                     {"chat_template_kwargs", {{"enable_thinking", thinking}}},
                     {"stream", true},
                     {"response_format", {{"type", "json_object"}}},
                     {"tools", {{{"type", "function"}, {"function", {{"name", "lookup"}}}}}},
                     {"tool_choice", "auto"}};
        const OpenAiChatRequest r = parsed(body.dump().c_str(), &ok);
        check(ok && r.stream && r.turn.think == thinking && r.turn.json_schema == R"({"type":"object"})" &&
                  r.turn.tools.size() == 1 && r.turn.tools[0].name == "lookup",
              std::string("JSON output keeps tools and chat_template_kwargs.enable_thinking=") +
                  (thinking ? "true" : "false"));
    }
    int format_shape = 0;
    for (const char * format :
         {"false", "7", "[]", R"j("json_object")j", R"j({"type":7})j", R"j({"type":{}})j", R"j({"type":"xml"})j",
          R"j({"type":"json_object","schema":false})j", R"j({"type":"json_object","schema":[]})j",
          R"j({"type":"json_object","schema":"{}"})j", R"j({"type":"json_schema","json_schema":7})j",
          R"j({"type":"json_schema","json_schema":[]})j", R"j({"type":"json_schema","json_schema":{"schema":true}})j",
          R"j({"type":"json_schema","json_schema":{"schema":[]}})j",
          R"j({"type":"json_schema","json_schema":{"schema":"{}"}})j",
          R"j({"type":"json_schema","json_schema":{"name":7}})j",
          R"j({"type":"json_schema","json_schema":{"description":false}})j",
          R"j({"type":"json_schema","json_schema":{"strict":"yes"}})j"}) {
        const std::string body =
            std::string(R"j({"messages":[{"role":"user","content":"hi"}],"response_format":)j") + format + "}";
        bool threw = false;
        try {
            parsed(body.c_str(), &ok, &err);
        } catch (const std::exception &) {
            threw = true;
        }
        check(!threw && !ok && err.find("response_format") != std::string::npos,
              "an unsupported response_format is refused, not ignored or thrown (#" + std::to_string(++format_shape) +
                  ")");
    }
    for (const char * schema : {"true", "false", "7", "[]", R"j("{}")j"}) {
        const std::string body =
            std::string(R"j({"messages":[{"role":"user","content":"hi"}],"json_schema":)j") + schema + "}";
        bool threw = false;
        try {
            parsed(body.c_str(), &ok, &err);
        } catch (const std::exception &) {
            threw = true;
        }
        check(!threw && !ok && err.find("json_schema") != std::string::npos,
              "a top-level schema of the wrong shape is refused: " + std::string(schema));
    }

    // The token budget is bounded where it enters: by the parameter table, not by what an int holds.
    const auto budget = [&](const char * field) {
        const std::string body = std::string(R"j({"messages":[{"role":"user","content":"hi"}],)j") + field + "}";
        return parsed(body.c_str(), &ok, &err).turn.n_predict;
    };
    const int top = (int) find_param("n-predict")->max;
    check(budget(R"j("max_tokens":7)j") == 7 && ok, "a budget inside the bound is taken as given");
    check(budget(R"j("max_tokens":2147483647)j") == top && ok, "a budget past the bound is clamped to it");
    check(budget(R"j("max_tokens":18446744073709551615)j") == top && ok, "and so is one no int could hold");
    check(budget(R"j("max_completion_tokens":5,"max_tokens":9)j") == 5 && ok, "max_completion_tokens wins");
    check(budget(R"j("max_tokens":null)j") == 100 && ok && budget(R"j("max_tokens":0)j") == 100 && ok,
          "null and 0 leave the configured budget");
    budget(R"j("max_tokens":-1)j");
    check(!ok && err.find("max_tokens") != std::string::npos, "a negative budget is refused");
    budget(R"j("max_tokens":12.5)j");
    check(!ok, "a fractional budget is refused");
    budget(R"j("max_completion_tokens":"100")j");
    check(!ok && err.find("max_completion_tokens") != std::string::npos, "a budget that is not a number is refused");

    // Responses.
    RunResult r;
    r.ok = true;
    r.generated_text = "";
    r.tool_calls = {{"", "read", "{\"path\":\"a.py\"}"}, {"given", "grep", "{}"}};
    r.summary.n_generated = 12;
    r.summary.n_prompt = 30; // prefilled this turn
    r.summary.n_past = 512;  // context after the turn
    ensure_tool_call_ids(r.tool_calls, "chatcmpl-9");
    check(r.tool_calls[0].id == "call_chatcmpl-9_0" && r.tool_calls[1].id == "given",
          "a call without an id gets one, and a given id is kept");
    check(finish_reason(r, 100) == "tool_calls", "a turn that calls finishes with tool_calls");
    check(finish_reason(r, r.summary.n_generated) == "length",
          "parsed tool calls cannot hide an exhausted token budget");
    const ChatResponseMeta meta{"chatcmpl-9", 5, "m.gguf"};
    const json c = completion_json(meta, r, finish_reason(r, 100));
    const json & msg = c["choices"][0]["message"];
    check(msg["content"].is_null() && msg["tool_calls"].size() == 2 &&
              msg["tool_calls"][0]["function"]["arguments"] == "{\"path\":\"a.py\"}" &&
              msg["tool_calls"][0]["type"] == "function" && !msg["tool_calls"][0].contains("index"),
          "a completion carries the calls, with null content");
    check(c["usage"]["prompt_tokens"] == 500 && c["usage"]["completion_tokens"] == 12 &&
              c["usage"]["total_tokens"] == 512 && c["usage"]["prompt_tokens_details"]["cached_tokens"] == 470,
          "usage counts the whole prompt and reports the reused part as cached");
    const json d = tool_calls_delta(r.tool_calls);
    check(d["tool_calls"][1]["index"] == 1 && d["tool_calls"][1]["id"] == "given", "a streamed call carries its index");
    const json uc = usage_chunk_json(meta, r.summary);
    check(uc["choices"].empty() && uc["usage"]["total_tokens"] == 512, "the usage chunk has no choices");

    RunResult plain;
    plain.generated_text = "hi";
    plain.summary.n_generated = 3;
    check(finish_reason(plain, 3) == "length" && finish_reason(plain, 4) == "stop", "stop or length by the budget");
    // Every failure kind has its own stable name, and only Failed closes the session.
    {
        const RunError kinds[] = {
            RunError::None,          RunError::ContextFull,    RunError::EmptyPrompt, RunError::TemplateRejected,
            RunError::NeedsChatMode, RunError::InvalidRequest, RunError::Failed,      RunError::InvalidOutput};
        std::set<std::string> names;
        for (RunError k : kinds)
            names.insert(run_error_name(k));
        check(names.size() == sizeof(kinds) / sizeof(kinds[0]), "every failure kind has its own name");
        check(std::string(run_error_name(RunError::ContextFull)) == "context_full", "a full context is context_full");
        check(!run_error_recoverable(RunError::Failed) && !run_error_recoverable(RunError::None) &&
                  run_error_recoverable(RunError::ContextFull) && run_error_recoverable(RunError::TemplateRejected),
              "only a failed run closes the session");
    }
    plain.summary.n_predict = 3;
    check(finish_reason(plain, 1000) == "length", "a budget the context cut short is a length stop");
    plain.summary.n_predict = 8;
    check(finish_reason(plain, 1000) == "stop", "an answer that ended under the cut budget is a stop");
    plain.summary.n_predict = 0;
    plain.cancelled = true;
    check(finish_reason(plain, 4) == "cancelled", "a cancelled turn says so");
    check(completion_json(meta, plain, "stop")["choices"][0]["message"]["content"] == "hi" &&
              !completion_json(meta, plain, "stop")["choices"][0]["message"].contains("tool_calls"),
          "an ordinary answer has text and no calls");
    RunResult partial;
    partial.ok = true;
    partial.generated_text = R"({"city":")";
    partial.reasoning_text = "The city is known.";
    partial.summary.n_predict = 3;
    partial.summary.n_generated = 3;
    const json limited = completion_json(meta, partial, finish_reason(partial, 10));
    check(limited["choices"][0]["finish_reason"] == "length" &&
              limited["choices"][0]["message"]["content"] == partial.generated_text &&
              limited["choices"][0]["message"]["reasoning_content"] == partial.reasoning_text,
          "a truncated JSON answer keeps its partial content, separate reasoning and length reason");
    partial.cancelled = true;
    const json cancelled = chunk_json(meta, json::object(), finish_reason(partial, 10));
    check(cancelled["choices"][0]["finish_reason"] == "cancelled" &&
              completion_json(meta, partial, finish_reason(partial, 10))["choices"][0]["message"]["content"] ==
                  partial.generated_text,
          "cancellation keeps its real reason and does not repair incomplete JSON");
}

// A stream's frames, as a client reads them: each event's name and its data.
static std::vector<std::pair<std::string, json>> sse_frames(const std::string & s) {
    std::vector<std::pair<std::string, json>> out;
    size_t at = 0;
    while (at < s.size()) {
        const size_t end = s.find("\n\n", at);
        if (end == std::string::npos) break;
        const std::string frame = s.substr(at, end - at);
        at = end + 2;
        const size_t nl = frame.find('\n');
        if (frame.rfind("event: ", 0) != 0 || nl == std::string::npos) continue;
        out.emplace_back(frame.substr(7, nl - 7), json::parse(frame.substr(nl + 1 + 6)));
    }
    return out;
}

static std::string names_of(const std::vector<std::pair<std::string, json>> & frames) {
    std::string s;
    for (const auto & f : frames)
        s += (s.empty() ? "" : " ") + f.first;
    return s;
}

static void block_stream_tests() {
    using T = BlockEvent::Type;
    BlockStream b;
    std::vector<BlockEvent> e = b.update("", "Let me");
    check(e.size() == 2 && e[0].type == T::Open && e[0].kind == BlockKind::Reasoning && e[1].text == "Let me",
          "the first reasoning opens a thinking block");
    e = b.update("", "Let me think");
    check(e.size() == 1 && e[0].type == T::Delta && e[0].text == " think", "a snapshot that extends sends the rest");
    e = b.update("Hi", "Let me think");
    check(e.size() == 3 && e[0].type == T::Close && e[0].kind == BlockKind::Reasoning && e[1].type == T::Open &&
              e[1].kind == BlockKind::Text && e[2].text == "Hi",
          "the answer closes the thinking and opens a text block");
    e = b.update("H", "Let me think");
    check(e.empty() && !b.diverged(), "a snapshot that takes text back sends nothing, and waits");
    e = b.update("Hi th\xC3", "Let me think");
    check(e.size() == 1 && e[0].text == " th", "half a UTF-8 character is held back");
    e = b.finish("Hi th\xC3\xA9re", "Let me think");
    check(e.size() == 2 && e[0].text == "\xC3\xA9re" && e[1].type == T::Close && !b.diverged(),
          "finish sends the rest of the final answer and closes the block");

    BlockStream d;
    d.update("<think>abc", "");
    e = d.finish("answer", "abc");
    check(d.diverged() && e.size() == 4 && e[0].type == T::Close && e[1].kind == BlockKind::Reasoning &&
              e[2].text == "abc",
          "a final answer that contradicts the stream is not sent over it, and is reported");
    BlockStream empty;
    check(empty.finish("", "").empty(), "a turn with no text has no blocks");

    std::vector<ChatMessage> m = {{"system", "a"}, {"system", "b"}, {"user", "q"}, {"system", "env"}, {"user", "r"}};
    place_system_messages(m);
    check(m.size() == 4 && m[0].role == "system" && m[0].content == "a\n\nb" && m[2].role == "user" &&
              m[2].content == "env",
          "leading system messages join; a later one is a user message, in its place");
    ChatRequest est;
    est.messages = {{"user", std::string(3000, 'x')}};
    check(estimate_prompt_tokens(est) > 900 && estimate_prompt_tokens(est) < 1100, "the estimate grows with the text");
}

static AnthropicRequest
anthropic(const std::string & body, bool * ok = nullptr, std::string * err = nullptr, bool think = false) {
    AnthropicRequest r;
    std::string e;
    RunConfig cfg;
    cfg.n_predict = 100;
    cfg.think = think;
    const bool good = parse_messages_request(json::parse(body), cfg, r, e);
    if (ok) *ok = good;
    if (err) *err = e;
    return r;
}

static void anthropic_tests() {
    bool ok = false;
    std::string err;
    {
        // What Claude Code sends: a metadata block first in system, text blocks, a system turn
        // after the first user message, a deferred tool, adaptive thinking.
        const AnthropicRequest r = anthropic(R"j({"model":"m","max_tokens":32000,"stream":true,
            "system":[{"type":"text","text":"x-anthropic-billing-header: cc_version=2.1.1.abc; cc_entrypoint=cli;"},
                      {"type":"text","text":"You are an agent.","cache_control":{"type":"ephemeral"}}],
            "messages":[{"role":"user","content":[{"type":"text","text":"<reminder/>"},{"type":"text","text":"do it"}]},
                        {"role":"system","content":[{"type":"text","text":"env"}]}],
            "tools":[{"name":"Write","description":"Write a file","input_schema":{"type":"object"}},
                     {"name":"Later","description":"x","input_schema":{"type":"object"},"defer_loading":true}],
            "thinking":{"type":"adaptive","display":"omitted"},"temperature":0.5,"stop_sequences":["x"],
            "metadata":{"user_id":"u"},"context_management":{"edits":[]}})j",
                                             &ok, &err);
        check(ok && r.stream && r.turn.n_predict == 32000, "a Claude Code request parses: " + err);
        check(r.turn.messages.size() == 3 && r.turn.messages[0].role == "system" &&
                  r.turn.messages[0].content == "You are an agent." &&
                  r.turn.messages[1].content == "<reminder/>\n\ndo it" && r.turn.messages[2].role == "user" &&
                  r.turn.messages[2].content == "env",
              "the per-request metadata block is dropped, blocks join, a later system turn is a user message");
        check(r.turn.tools.size() == 1 && r.turn.tools[0].name == "Write" &&
                  r.turn.tools[0].parameters == R"({"type":"object"})" && r.turn.think,
              "client tools are described, a deferred one is not, adaptive thinking asks to think");
    }
    {
        const AnthropicRequest r = anthropic(R"j({"max_tokens":50,"messages":[
            {"role":"user","content":"read a.py"},
            {"role":"assistant","content":[{"type":"thinking","thinking":"need it","signature":""},
              {"type":"text","text":"Reading."},
              {"type":"tool_use","id":"t1","name":"Read","input":{"path":"a.py"}}]},
            {"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":[{"type":"text","text":"print(1)"}]},
              {"type":"text","text":"and now?"}]}],
            "tool_choice":{"type":"any"},"thinking":{"type":"disabled"}})j",
                                             &ok, &err, true);
        check(ok && r.turn.messages.size() == 4, "an agent's history parses: " + err);
        const ChatMessage & a = r.turn.messages[1];
        check(a.role == "assistant" && a.reasoning == "need it" && a.content == "Reading." &&
                  a.tool_calls.size() == 1 && a.tool_calls[0].id == "t1" &&
                  a.tool_calls[0].arguments == R"({"path":"a.py"})",
              "an assistant turn keeps its thinking, text and call");
        check(r.turn.messages[2].role == "tool" && r.turn.messages[2].tool_call_id == "t1" &&
                  r.turn.messages[2].name == "Read" && r.turn.messages[2].content == "print(1)" &&
                  r.turn.messages[3].role == "user" && r.turn.messages[3].content == "and now?",
              "a tool result is a tool message, named after its call, before the user's text");
        check(r.turn.tool_choice == ToolChoice::Required && !r.turn.think && r.turn.n_predict == 50 && !r.stream,
              "any is required, thinking can be turned off, max_tokens is the budget");
    }
    {
        const AnthropicRequest r = anthropic(R"j({"messages":[{"role":"user","content":[
            {"type":"tool_result","tool_use_id":"t9","content":[{"type":"tool_reference","tool_name":"WebFetch"}]}]}]})j",
                                             &ok, &err);
        check(ok && r.turn.messages.size() == 1 && r.turn.messages[0].content == "Tool loaded: WebFetch" &&
                  r.turn.n_predict == 100,
              "a tool search result reads as text, and no max_tokens is the configured budget");
    }
    anthropic(R"j({"messages":[{"role":"user","content":[{"type":"image","source":{}}]}]})j", &ok, &err);
    check(!ok && err.find("image") != std::string::npos, "an image is refused, not dropped");
    anthropic(
        R"j({"messages":[{"role":"user","content":"q"}],"tools":[{"type":"web_search_20250305","name":"web_search"}]})j",
        &ok, &err);
    check(!ok && err.find("server tool") != std::string::npos, "a server tool is refused");
    anthropic(R"j({"messages":[{"role":"user","content":"q"},{"role":"assistant","content":"The answer is"}]})j", &ok,
              &err);
    check(!ok && err.find("prefilled") != std::string::npos, "a prefilled answer is refused");
    anthropic(R"j({"messages":[{"role":7,"content":"q"}]})j", &ok, &err);
    check(!ok, "a role that is not a string is refused, not thrown");
    anthropic(R"j({"messages":[{"role":"user","content":"q"}],"max_tokens":-1})j", &ok, &err);
    check(!ok && err.find("max_tokens") != std::string::npos, "a negative budget is refused");
    {
        const AnthropicRequest r = anthropic(R"j({"messages":[{"role":"user","content":"q"}],
            "output_config":{"format":{"type":"json_schema","schema":{"type":"object"}}}})j",
                                             &ok);
        check(ok && r.turn.json_schema == R"({"type":"object"})", "output_config.format constrains the answer");
    }

    const json full = anthropic_error_json(400, "the prompt does not fit", "context_full");
    check(full["type"] == "error" && full["error"]["type"] == "invalid_request_error" &&
              full["error"]["message"].get<std::string>().rfind("prompt is too long", 0) == 0,
          "a full context is the protocol's prompt-is-too-long, which a client compacts on");
    check(anthropic_error_json(503, "no model", "no_model")["error"]["type"] == "overloaded_error" &&
              anthropic_error_json(401, "key", "")["error"]["type"] == "authentication_error" &&
              anthropic_error_json(500, "x", "failed")["error"]["type"] == "api_error",
          "the error type follows the status");
    check(anthropic_stop_reason("stop") == "end_turn" && anthropic_stop_reason("length") == "max_tokens" &&
              anthropic_stop_reason("tool_calls") == "tool_use",
          "stop reasons in the protocol's words");

    RunResult r;
    r.ok = true;
    r.reasoning_text = "think";
    r.generated_text = "Writing.";
    r.tool_calls = {{"", "Write", R"({"path":"h.txt"})"}};
    r.summary.n_generated = 12;
    r.summary.n_prompt = 30;
    r.summary.n_past = 512;
    ensure_tool_call_ids(r.tool_calls, "msg_4");
    const ChatResponseMeta meta{"msg_4", 5, "m.gguf"};
    const json m = message_json(meta, r, "tool_calls");
    check(m["type"] == "message" && m["content"].size() == 3 && m["content"][0]["type"] == "thinking" &&
              m["content"][1]["text"] == "Writing." && m["content"][2]["type"] == "tool_use" &&
              m["content"][2]["input"]["path"] == "h.txt" && m["stop_reason"] == "tool_use",
          "a message is thinking, text, then the calls with their input as an object");
    check(m["usage"]["input_tokens"] == 30 && m["usage"]["cache_read_input_tokens"] == 470 &&
              m["usage"]["output_tokens"] == 12,
          "usage splits the prompt into what was prefilled and what the KV held");

    AnthropicStream s(meta);
    std::string wire = s.start();
    wire += s.on_token("", "thi");
    wire += s.on_token("", "think");
    wire += s.on_token("Writ", "think");
    wire += AnthropicStream::ping();
    wire += s.finish(r, "tool_calls");
    const auto f = sse_frames(wire);
    check(names_of(f) == "message_start content_block_start content_block_delta content_block_delta "
                         "content_block_delta content_block_stop content_block_start content_block_delta ping "
                         "content_block_delta content_block_stop content_block_start content_block_delta "
                         "content_block_stop message_delta message_stop",
          "the stream: message_start, a thinking block (signed), a text block, a tool_use block, the stop");
    check(f[1].second["content_block"]["type"] == "thinking" && f[3].second["delta"]["thinking"] == "nk" &&
              f[4].second["delta"]["type"] == "signature_delta" && f[6].second["index"] == 1 &&
              f[9].second["delta"]["text"] == "ing." && f[11].second["index"] == 2 &&
              f[11].second["content_block"]["id"] == "call_msg_4_0" &&
              json::parse(f[12].second["delta"]["partial_json"].get<std::string>())["path"] == "h.txt",
          "blocks are indexed in order, and the call's input arrives as JSON");
    check(f[14].second["delta"]["stop_reason"] == "tool_use" && f[14].second["usage"]["output_tokens"] == 12,
          "message_delta carries the stop reason and the usage");
    const auto e = sse_frames(AnthropicStream::error(400, "too long", "context_full"));
    check(e.size() == 1 && e[0].first == "error" && e[0].second["error"]["type"] == "invalid_request_error",
          "a failed turn ends the stream with an error event");
}

static ResponsesRequest
responses(const std::string & body, bool * ok = nullptr, std::string * err = nullptr, bool think = false) {
    ResponsesRequest r;
    std::string e;
    RunConfig cfg;
    cfg.n_predict = 100;
    cfg.think = think;
    const bool good = parse_responses_request(json::parse(body), cfg, r, e);
    if (ok) *ok = good;
    if (err) *err = e;
    return r;
}

static void responses_tests() {
    bool ok = false;
    std::string err;
    {
        // What Codex sends: developer messages first, built-in tools beside the functions, store off.
        const ResponsesRequest r = responses(R"j({"model":"m","stream":true,"store":false,"instructions":"Be precise.",
            "input":[{"type":"message","role":"developer","content":[{"type":"input_text","text":"perms"}]},
                     {"type":"message","role":"user","content":[{"type":"input_text","text":"env"}]},
                     {"type":"message","role":"user","content":[{"type":"input_text","text":"make hello.txt"}]}],
            "tools":[{"type":"function","name":"exec_command","description":"Run","strict":false,
                      "parameters":{"type":"object","properties":{"cmd":{"type":"string"}}}},
                     {"type":"web_search","external_web_access":false},
                     {"type":"namespace","name":"agents","tools":[]}],
            "tool_choice":"auto","parallel_tool_calls":true,"reasoning":{"summary":"auto"},
            "include":["reasoning.encrypted_content"],"prompt_cache_key":"k","max_output_tokens":64})j",
                                             &ok, &err, true);
        check(ok && r.stream && r.turn.n_predict == 64 && r.turn.think, "a Codex request parses: " + err);
        check(r.turn.messages.size() == 3 && r.turn.messages[0].role == "system" &&
                  r.turn.messages[0].content == "Be precise.\n\nperms" &&
                  r.turn.messages[2].content == "make hello.txt",
              "instructions and the leading developer messages are one system message");
        check(r.turn.tools.size() == 1 && r.turn.tools[0].name == "exec_command" &&
                  r.turn.tool_choice == ToolChoice::Auto,
              "function tools are described, built-in ones dropped");
    }
    {
        const ResponsesRequest r = responses(R"j({"input":[
            {"role":"user","content":"make it"},
            {"type":"reasoning","id":"rs_1","summary":[],"content":[{"type":"reasoning_text","text":"shell"}]},
            {"type":"message","role":"assistant","content":[{"type":"output_text","text":"Running."}]},
            {"type":"function_call","call_id":"c1","name":"exec_command","arguments":"{\"cmd\":\"ls\"}"},
            {"type":"function_call_output","call_id":"c1","output":"a.txt"},
            {"type":"message","role":"developer","content":"late note"}],
            "reasoning":{"effort":"none"},"tool_choice":{"type":"function","name":"exec_command"}})j",
                                             &ok, &err, true);
        check(ok && r.turn.messages.size() == 4, "a Codex history parses: " + err);
        const ChatMessage & a = r.turn.messages[1];
        check(a.role == "assistant" && a.reasoning == "shell" && a.content == "Running." && a.tool_calls.size() == 1 &&
                  a.tool_calls[0].id == "c1" && a.tool_calls[0].arguments == R"({"cmd":"ls"})",
              "a turn's reasoning, text and call are one assistant message");
        check(r.turn.messages[2].role == "tool" && r.turn.messages[2].name == "exec_command" &&
                  r.turn.messages[2].content == "a.txt" && r.turn.messages[3].role == "user" &&
                  r.turn.messages[3].content == "late note",
              "a call's output is a tool message; a later developer message a user one");
        check(!r.turn.think && r.turn.tool_choice == ToolChoice::Required && r.turn.n_predict == 100,
              "effort none turns thinking off; a named function is required");
    }
    check(responses(R"j({"input":"hi"})j", &ok).turn.messages.size() == 1 && ok, "a string input is one user message");
    responses(R"j({"input":"hi","previous_response_id":"resp_1"})j", &ok, &err);
    check(!ok && err.find("previous_response_id") != std::string::npos, "previous_response_id is refused clearly");
    responses(R"j({"input":[{"type":"item_reference","id":"x"}]})j", &ok, &err);
    check(!ok && err.find("stores none") != std::string::npos, "a stored item reference is refused");
    responses(R"j({"input":[{"role":"user","content":[{"type":"input_image","image_url":"x"}]}]})j", &ok, &err);
    check(!ok && err.find("input_image") != std::string::npos, "an image is refused, not dropped");
    responses(R"j({"input":"hi","background":true})j", &ok, &err);
    check(!ok, "a background response is refused");
    responses(R"j({"input":[{"role":"user","content":"q"},{"role":"assistant","content":"a"}]})j", &ok, &err);
    check(!ok, "an input that ends on the assistant is refused");
    responses(R"j({"input":7})j", &ok, &err);
    check(!ok, "an input that is neither text nor items is refused");
    check(responses(R"j({"input":"q","text":{"format":{"type":"json_schema","name":"x","schema":{"type":"object"}}}})j",
                    &ok)
                      .turn.json_schema == R"({"type":"object"})" &&
              ok,
          "text.format constrains the answer");

    const json full = responses_error_json(400, "too long", "context_full");
    check(full["error"]["code"] == "context_length_exceeded" && full["error"]["type"] == "invalid_request_error",
          "a full context has the code a client knows");

    RunResult r;
    r.ok = true;
    r.reasoning_text = "plan";
    r.generated_text = "Done.";
    r.tool_calls = {{"", "exec_command", R"({"cmd":"ls"})"}};
    r.summary.n_generated = 12;
    r.summary.n_prompt = 30;
    r.summary.n_past = 512;
    ensure_tool_call_ids(r.tool_calls, "resp_3");
    const ChatResponseMeta meta{"resp_3", 5, "m.gguf"};
    const json o = response_json(meta, r, "tool_calls");
    check(o["object"] == "response" && o["status"] == "completed" && o["output"].size() == 3 &&
              o["output"][0]["type"] == "reasoning" && o["output"][1]["content"][0]["text"] == "Done." &&
              o["output"][2]["call_id"] == "call_resp_3_0" && o["usage"]["input_tokens"] == 500 &&
              o["usage"]["input_tokens_details"]["cached_tokens"] == 470,
          "a response lists reasoning, message and call items, with the usage");
    check(response_json(meta, r, "length")["status"] == "incomplete" &&
              response_json(meta, r, "length")["incomplete_details"]["reason"] == "max_output_tokens",
          "a turn that ran out of budget is incomplete");

    ResponsesStream s(meta);
    std::string wire = s.start();
    wire += s.on_token("", "pl");
    wire += s.on_token("Do", "plan");
    wire += s.finish(r, "tool_calls");
    const auto f = sse_frames(wire);
    check(names_of(f) == "response.created response.in_progress response.output_item.added "
                         "response.reasoning_text.delta response.reasoning_text.delta response.reasoning_text.done "
                         "response.output_item.done response.output_item.added response.content_part.added "
                         "response.output_text.delta response.output_text.delta response.output_text.done "
                         "response.content_part.done response.output_item.done response.output_item.added "
                         "response.function_call_arguments.delta response.function_call_arguments.done "
                         "response.output_item.done response.completed",
          "the stream: created, a reasoning item, a message item, a function_call item, completed");
    bool ordered = true;
    for (size_t i = 0; i < f.size(); ++i)
        ordered = ordered && f[i].second["sequence_number"] == (int) i && f[i].second["type"] == f[i].first;
    check(ordered, "every event names its type and is numbered in order");
    const json & done = f.back().second["response"];
    check(done["output"].size() == 3 && done["output"][1]["content"][0]["text"] == "Done." &&
              done["output"][2]["arguments"] == R"({"cmd":"ls"})" && done["usage"]["output_tokens"] == 12 &&
              f[13].second["output_index"] == 1 && f[17].second["item"]["call_id"] == "call_resp_3_0",
          "response.completed lists the items the stream closed, and the usage");
    ResponsesStream failed(meta);
    failed.start();
    const auto e = sse_frames(failed.error(400, "too long", "context_full"));
    check(e.size() == 1 && e[0].first == "response.failed" &&
              e[0].second["response"]["error"]["code"] == "context_length_exceeded",
          "a failed turn ends the stream with response.failed");
}

static void queue_tests() {
    using Admit = TurnQueue::Admit;
    const auto fast = std::chrono::milliseconds(5);
    {
        TurnQueue q(1);
        check(q.enter() == Admit::Granted, "an empty line is entered at once");
        std::vector<int> order;
        std::mutex om;
        std::thread second([&] {
            if (q.enter(nullptr, fast) == Admit::Granted) {
                std::lock_guard<std::mutex> g(om);
                order.push_back(2);
            }
            q.leave();
        });
        while (q.waiting() < 1)
            std::this_thread::sleep_for(fast);
        check(q.enter(nullptr, fast) == Admit::Full, "a full line turns the next caller away");
        {
            std::lock_guard<std::mutex> g(om);
            order.push_back(1);
        }
        q.leave();
        second.join();
        check(order == std::vector<int>({1, 2}), "the waiting caller runs after the running one");
        check(q.enter() == Admit::Granted, "the line is free again afterwards");
        q.leave();
    }
    {
        // Three callers behind the running one; the middle one gives up. The others still run, in
        // the order they arrived.
        TurnQueue q(8);
        q.enter();
        std::vector<int> order;
        std::mutex om;
        std::atomic<bool> give_up{false};
        std::vector<std::thread> ts;
        std::atomic<int> abandoned{0};
        for (int i = 0; i < 3; ++i) {
            ts.emplace_back([&, i] {
                const Admit a = q.enter([&, i] { return !(i == 1 && give_up); }, fast);
                if (a == Admit::Abandoned) {
                    ++abandoned;
                    return;
                }
                {
                    std::lock_guard<std::mutex> g(om);
                    order.push_back(i);
                }
                q.leave();
            });
            while (q.waiting() < (size_t) i + 1)
                std::this_thread::sleep_for(fast);
        }
        give_up = true;
        while (abandoned == 0)
            std::this_thread::sleep_for(fast);
        check(q.waiting() == 2, "a caller that gives up leaves the line");
        q.leave();
        for (std::thread & t : ts)
            t.join();
        check(order == std::vector<int>({0, 2}), "the line moves past the place that was given up");
        check(q.enter() == Admit::Granted, "and is free once everyone has run");
        q.leave();
    }
}

static void settings_tests() {
    RunConfig defaults;
    defaults.n_threads = 4;
    defaults.moe.enabled = true;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "bmoe_server_test_settings.json";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    SettingsStore s(defaults, path.u8string());
    check(s.load().empty(), "no settings file is a clean start");
    check(s.config().n_threads == 4, "defaults apply");

    s.apply_plan({{"threads", "6"}, {"io-threads", "2"}}, json::array());
    check(s.config().n_threads == 6 && s.config().moe.io_threads == 2, "plan layer overrides defaults");
    check(s.operator_keys().empty(), "a plan is not an operator choice");

    s.set_launch_values({{"threads", "8"}});
    check(s.config().n_threads == 8, "launch flags override the plan");
    check(s.launch_keys() == std::vector<std::string>{"threads"}, "launch keys: the launch layer's");

    json rejected = s.apply(json{{"threads", 12}, {"top-k", "abc"}, {"nope", 1}}, {});
    check(s.config().n_threads == 12, "user values override launch flags");
    check(rejected.contains("top-k") && rejected.contains("nope") && !rejected.contains("threads"),
          "unparseable values and unknown keys are rejected, the rest applied");
    const auto keys = s.operator_keys();
    check(keys.size() == 1 && keys[0] == "threads", "operator keys: launch and user");
    check(s.launch_keys().empty(), "launch keys: a value the person sets is theirs, no longer the launch's");

    s.apply(json::object(), {"threads"});
    check(s.config().n_threads == 6, "reset removes the key from every operator layer: the plan shows through");

    s.apply(json{{"cache-mb", "auto"}, {"temp", 0.7}, {"overlap", true}}, {});
    SettingsStore again(defaults, path.u8string());
    check(again.load().empty(), "saved settings reload without warnings");
    const RunConfig c = again.config();
    check(c.moe.cache_auto && c.sampling.temp == 0.7f && c.moe.overlap, "user layer persists with its types");
    check(again.auto_plan(true) && !again.auto_plan(false), "auto_plan follows the fallback until it is set");
    again.set_auto_plan(false);
    SettingsStore third(defaults, path.u8string());
    third.load();
    check(!third.auto_plan(true), "auto_plan, once turned off, stays off across a restart");

    std::filesystem::remove(path, ec);
}

static void planning_config_tests() {
    const std::string path = (std::filesystem::temp_directory_path() / "bmoe_server_test_plan.json").string();
    std::filesystem::remove(path);
    RunConfig defaults;
    defaults.moe.enabled = true;
    SettingsStore store(defaults, path);
    store.apply(json{{"threads", 6}}, {});
    store.apply_plan({{"cache-mb", "1333"}, {"force-cache", "true"}}, json::array());
    check(store.config().moe.cache_mb == 1333 && store.config().moe.force_cache && store.config().n_threads == 6,
          "the plan layer is in the config");
    const RunConfig base = store.planning_config();
    check(base.moe.cache_mb == defaults.moe.cache_mb && !base.moe.force_cache && base.n_threads == 6,
          "the planning config leaves the plan layer out and keeps the person's values");
    // What a second plan would do: its values are what differ from the planning config, so a
    // choice it repeats is still a value, and replacing the layer keeps it.
    std::map<std::string, std::string> second;
    RunConfig planned = base;
    planned.moe.cache_mb = 1233;
    planned.moe.force_cache = true;
    for (const ParamDesc & d : params())
        if (d.get(planned) != d.get(base)) second[d.key] = d.get(planned);
    check(second.count("force-cache") == 1 && second.count("cache-mb") == 1, "a repeated choice is still a value");
    store.apply_plan(second, json::array());
    // (validate() wants a model path too; the cache rule is the one this guards.)
    check(store.config().moe.cache_mb == 1233 && store.config().moe.force_cache &&
              validate(store.config()).error.find("pathological") == std::string::npos,
          "the second plan applies whole, and the cache rule is satisfied");
    std::filesystem::remove(path);
}

static void json_tests() {
    std::string s;
    check(value_to_param_string(json(true), s) && s == "true", "bool to parameter string");
    check(value_to_param_string(json(4), s) && s == "4", "int to parameter string");
    check(value_to_param_string(json(4.0), s) && s == "4", "integral float is an int");
    check(value_to_param_string(json(0.95), s) && s == "0.95", "float keeps its value");
    check(value_to_param_string(json("auto"), s) && s == "auto", "string passes through");
    check(!value_to_param_string(json::array(), s), "an array is not a parameter value");

    const ParamDesc * cache = find_param("cache-mb");
    check(param_string_to_value(*cache, "auto") == json("auto") && param_string_to_value(*cache, "2000") == json(2000),
          "an accepts-auto int renders as auto or a number");
    check(param_string_to_value(*find_param("moe-stream"), "true") == json(true), "bool renders as a JSON bool");

    // What a write to the API must declare. The three types a form can send are the ones to refuse.
    check(is_json_content_type("application/json") && is_json_content_type("Application/JSON; charset=utf-8") &&
              is_json_content_type(" application/json ;charset=UTF-8"),
          "JSON is JSON in any case, with or without parameters");
    check(!is_json_content_type("") && !is_json_content_type("text/plain") &&
              !is_json_content_type("application/x-www-form-urlencoded") &&
              !is_json_content_type("multipart/form-data; boundary=x"),
          "what a form can send is not JSON, and neither is no type at all");
    check(!is_json_content_type("application/jsonp") && !is_json_content_type("text/plain; application/json") &&
              !is_json_content_type("application/json, text/plain"),
          "a type that only contains the word is not JSON");

    const std::string object = u8"{\"city\":\"北京\",\"emoji\":\"🙂𐍈\",\"accent\":\"é\"}";
    const std::string reasoning = u8"城市是北京，réponse 🙂。";
    const ChatResponseMeta meta{"chatcmpl-utf8", 5, "m.gguf"};
    for (const auto & field : {std::pair<const char *, std::string>{"content", object},
                               std::pair<const char *, std::string>{"reasoning_content", reasoning}}) {
        std::string sent, reconstructed;
        bool exact = true;
        for (size_t count = 1; count <= field.second.size(); ++count) {
            const std::string raw = field.second.substr(0, count);
            const std::string visible = raw.substr(0, complete_utf8_prefix_size(raw));
            const std::string delta = visible.substr(sent.size());
            try {
                const json chunk = chunk_json(meta, json{{field.first, delta}}, nullptr);
                // Strict serialization must also work: replacement must never be needed for a
                // completed code point, even when every byte arrived from a different token.
                const std::string strict = chunk.dump();
                const std::string replacing = chunk.dump(-1, ' ', false, json::error_handler_t::replace);
                exact = exact && strict == replacing;
                reconstructed += json::parse(replacing)["choices"][0]["delta"][field.first].get<std::string>();
            } catch (const std::exception &) {
                exact = false;
            }
            sent = visible;
        }
        check(exact && reconstructed == field.second,
              std::string("SSE ") + field.first + " reconstructs Chinese, emoji and accented text across byte tokens");
    }
    check(complete_utf8_prefix_size("") == 0 && complete_utf8_prefix_size("ASCII") == 5 &&
              complete_utf8_prefix_size(std::string("a\xE4\xB8", 3)) == 1 &&
              complete_utf8_prefix_size(std::string("a\xF0\x9F\x99", 4)) == 1,
          "incomplete final code points wait without hiding complete ASCII");
    check(complete_utf8_prefix_size(std::string("a\xC0\x80", 3)) == 1 &&
              complete_utf8_prefix_size(std::string("a\xED\xA0\x80", 4)) == 1 &&
              complete_utf8_prefix_size(std::string("a\xF4\x90\x80\x80", 5)) == 1 &&
              complete_utf8_prefix_size(std::string("a\xE4x", 3)) == 1 &&
              complete_utf8_prefix_size(std::string("a\xF4\x8F\xBF\xBF", 5)) == 5,
          "UTF-8 boundaries reject invalid, overlong, surrogate and out-of-range encodings");
}

static void access_tests() {
    check(is_api_path("/api/info") && is_api_path("/v1/chat/completions") && !is_api_path("/") &&
              !is_api_path("/assets/app.js") && !is_api_path("/apix"),
          "the API is /api/* and /v1/*, nothing else");
    check(is_loopback_host("127.0.0.1") && is_loopback_host("localhost:8765") && is_loopback_host("[::1]:8765") &&
              is_loopback_host("::1") && !is_loopback_host("0.0.0.0") && !is_loopback_host("example.com:8765") &&
              !is_loopback_host("192.168.1.10"),
          "loopback host names, with or without a port");
    check(browse_url("127.0.0.1", 8765) == "http://127.0.0.1:8765/" &&
              browse_url("0.0.0.0", 8765) == "http://127.0.0.1:8765/" &&
              browse_url("::", 8765) == "http://[::1]:8765/" && browse_url("::1", 1) == "http://[::1]:1/" &&
              browse_url("[::1]", 1) == "http://[::1]:1/" && browse_url("fe80::2", 80) == "http://[fe80::2]:80/" &&
              browse_url("localhost", 80) == "http://localhost:80/",
          "the URL a person opens: a wildcard bind through loopback, an IPv6 literal in brackets");

    check(bearer_matches("Bearer s3cret", "s3cret") && bearer_matches("bearer s3cret", "s3cret"),
          "a bearer token matches the key, the scheme in either case");
    check(!bearer_matches("Bearer s3cre", "s3cret") && !bearer_matches("Bearer s3cret1", "s3cret") &&
              !bearer_matches("Bearer S3CRET", "s3cret") && !bearer_matches("s3cret", "s3cret") &&
              !bearer_matches("Basic s3cret", "s3cret") && !bearer_matches("", "s3cret"),
          "a shorter, longer, differently cased, bare or basic token does not");
    check(!bearer_matches("Bearer ", "") && !bearer_matches("", ""), "an empty key matches nothing");

    AccessPolicy local;
    local.loopback = true;
    local.origins = {"http://127.0.0.1:8765"};
    auto req = [](const char * method, const char * path) {
        RequestFacts f;
        f.method = method;
        f.path = path;
        f.host = "127.0.0.1:8765";
        return f;
    };
    check(check_access(local, req("GET", "/api/info")).allowed(), "a local program's GET is answered");
    {
        RequestFacts f = req("POST", "/api/cancel");
        f.content_type = "application/json";
        check(check_access(local, f).allowed(), "a local program's JSON write is answered");
        f.content_type = "text/plain";
        check(check_access(local, f).status == 415, "a write that is not JSON is refused with 415");
    }
    {
        RequestFacts f = req("GET", "/api/info");
        f.host = "evil.example:8765";
        check(check_access(local, f).status == 403, "a loopback server refuses a foreign host name");
        AccessPolicy open = local;
        open.loopback = false;
        check(check_access(open, f).allowed(), "a server bound beyond loopback answers to its name");
    }
    {
        RequestFacts f = req("GET", "/api/plan");
        f.has_origin = true;
        f.origin = "http://evil.example";
        check(check_access(local, f).status == 403, "a cross-origin GET to the API is refused");
        f.origin = "http://127.0.0.1:8765";
        check(check_access(local, f).allowed(), "the UI's own origin is answered");
        RequestFacts g = req("GET", "/api/events");
        g.sec_fetch_site = "cross-site";
        check(check_access(local, g).status == 403, "a cross-site read with no Origin is refused too");
        RequestFacts h = req("GET", "/assets/app.js");
        h.sec_fetch_site = "cross-site";
        check(check_access(local, h).allowed(), "the UI's files are not the API");
        // Another port of the same host is the same site, and exactly where a hostile local page is.
        for (const char * site : {"same-site", "cross-site", "something-new"}) {
            RequestFacts s = req("GET", "/api/plan");
            s.sec_fetch_site = site;
            check(check_access(local, s).status == 403,
                  std::string("Sec-Fetch-Site: ") + site + " is refused on the API");
        }
        for (const char * site : {"same-origin", "none", ""}) {
            RequestFacts s = req("GET", "/api/plan");
            s.sec_fetch_site = site;
            check(check_access(local, s).allowed(),
                  std::string("Sec-Fetch-Site: '") + site + "' (this page, the person, or no browser) is answered");
        }
        RequestFacts so = req("GET", "/api/plan");
        so.sec_fetch_site = "same-site";
        so.has_origin = true;
        so.origin = "http://127.0.0.1:8765";
        check(check_access(local, so).allowed(), "an Origin the server knows decides over Sec-Fetch-Site");
    }
    {
        AccessPolicy keyed = local;
        keyed.api_key = "s3cret";
        RequestFacts f = req("GET", "/v1/models");
        const AccessDecision d = check_access(keyed, f);
        check(d.status == 401, "with a key set, an API request without it gets 401");
        f.authorization = "Bearer s3cret";
        check(check_access(keyed, f).allowed(), "and is answered with it");
        f.authorization = "Bearer wrong";
        check(check_access(keyed, f).status == 401, "a wrong key is 401");
        RequestFacts a = req("POST", "/v1/messages");
        a.content_type = "application/json";
        a.x_api_key = "s3cret";
        check(check_access(keyed, a).allowed(), "the key is also taken as X-Api-Key, as Anthropic clients send it");
        a.x_api_key = "s3cre";
        check(check_access(keyed, a).status == 401, "and a wrong one there is 401");
        RequestFacts page = req("GET", "/index.html");
        check(check_access(keyed, page).allowed(), "the key gates the API, not the UI's files");
        RequestFacts w = req("PUT", "/api/config");
        w.authorization = "Bearer s3cret";
        check(check_access(keyed, w).status == 415, "the key does not excuse a write from naming JSON");
        RequestFacts x = req("POST", "/v1/chat/completions");
        x.has_origin = true;
        x.origin = "http://evil.example";
        x.authorization = "Bearer s3cret";
        check(check_access(keyed, x).status == 403, "a foreign origin is refused before the key is looked at");
    }

    check(launch_refusal("127.0.0.1", "", false).empty() && launch_refusal("localhost", "", false).empty() &&
              launch_refusal("::1", "", false).empty(),
          "loopback needs no key");
    check(!launch_refusal("0.0.0.0", "", false).empty() && !launch_refusal("192.168.1.10", "", false).empty(),
          "beyond loopback without a key the server does not start");
    check(launch_refusal("0.0.0.0", "s3cret", false).empty() && launch_refusal("0.0.0.0", "", true).empty(),
          "a key, or --no-api-key, lets it");

    check(is_loopback_peer("127.0.0.1") && is_loopback_peer("127.8.9.10") && is_loopback_peer("::1") &&
              is_loopback_peer("::ffff:127.0.0.1"),
          "a loopback peer, IPv4, IPv6 or mapped");
    check(!is_loopback_peer("192.168.1.10") && !is_loopback_peer("::ffff:10.0.0.2") && !is_loopback_peer("") &&
              !is_loopback_peer("fe80::1") && !is_loopback_peer("1127.0.0.1"),
          "anything else is the network");

    const auto csp = [] {
        for (const auto & h : security_headers())
            if (h.first == "Content-Security-Policy") return h.second;
        return std::string();
    }();
    check(csp.find("img-src 'self' data:;") != std::string::npos &&
              csp.find("frame-ancestors 'none'") != std::string::npos &&
              csp.find("script-src 'self';") != std::string::npos && csp.find("unsafe-") == std::string::npos,
          "the page loads images from itself or data: only, runs no inline script and cannot be framed");
    std::set<std::string> names;
    for (const auto & h : security_headers())
        names.insert(h.first);
    check(names.count("X-Content-Type-Options") && names.count("Referrer-Policy"), "no sniffing, no referrer");
}

static void file_tests() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("bmoe-server-test-" + std::to_string(std::rand()));
    fs::create_directories(dir / "models" / "sub");
    const auto write = [](const fs::path & p, const char * s) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::FILE * f = std::fopen(p.u8string().c_str(), "wb");
        check(f != nullptr, "the test can write its fixture");
        if (!f) return;
        std::fputs(s, f);
        std::fclose(f);
    };

    std::string key, err;
    write(dir / "key", "s3cret\r\n");
    check(read_api_key_file((dir / "key").u8string(), key, err) && key == "s3cret",
          "a key file is read without its trailing line break");
    write(dir / "blank", " \n\n");
    key = "kept";
    check(!read_api_key_file((dir / "blank").u8string(), key, err) && !err.empty() && key == "kept",
          "a key file with no key is refused");
    check(!read_api_key_file((dir / "absent").u8string(), key, err) && !err.empty(), "and so is a missing one");

    const std::vector<std::string> models = {(dir / "models").u8string()};
    write(dir / "models" / "a.gguf", "");
    write(dir / "secret.gguf", "");
    std::string out;
    check(confine_model_path((dir / "models" / "a.gguf").u8string(), models, out, err) &&
              fs::equivalent(fs::u8path(out), dir / "models" / "a.gguf"),
          "a model in the models folder is opened");
    check(confine_model_path("sub/b.gguf", models, out, err) &&
              fs::u8path(out).lexically_relative(fs::weakly_canonical(dir / "models")) == fs::path("sub/b.gguf"),
          "a relative path is taken inside the models folder");
    check(!confine_model_path((dir / "secret.gguf").u8string(), models, out, err) && !err.empty(),
          "a file outside it is refused");
    check(!confine_model_path((dir / "models" / ".." / "secret.gguf").u8string(), models, out, err),
          "and so is a way out through ..");
    check(!confine_model_path("../secret.gguf", models, out, err), "relative too");
    check(!confine_model_path(models.front(), models, out, err), "the folder itself is not a model");
    std::error_code ec;
    fs::create_symlink(dir / "secret.gguf", dir / "models" / "link.gguf", ec);
    if (!ec)
        check(!confine_model_path((dir / "models" / "link.gguf").u8string(), models, out, err),
              "a link inside the folder to a file outside it is refused");
    check(confine_model_path("", models, out, err) && out.empty(), "no model is no path to confine");
    // A folder the server lists models from beside the models folder (--scan-dir) is a root too, and
    // only the models folder takes relative paths.
    write(dir / "elsewhere" / "c.gguf", "");
    const std::vector<std::string> roots = {models.front(), (dir / "elsewhere").u8string()};
    check(confine_model_path((dir / "elsewhere" / "c.gguf").u8string(), roots, out, err) &&
              fs::equivalent(fs::u8path(out), dir / "elsewhere" / "c.gguf"),
          "a model in a scanned folder is opened");
    check(!confine_model_path((dir / "elsewhere" / "c.gguf").u8string(), models, out, err),
          "and refused where that folder is not listed");
    check(!confine_model_path((dir / "secret.gguf").u8string(), roots, out, err), "a second root widens nothing else");
    check(!confine_model_path("x.gguf", {}, out, err) && !err.empty(), "no folder at all opens nothing");

    // The scan goes one folder deep (a split set kept in a folder of its own), and no further: not
    // into a folder inside that one, and not through a link to a folder, which could loop or lead
    // out of the root. What the listing shows is still opened only through the confinement.
    write(dir / "models" / "set" / "big-00001-of-00002.gguf", "");
    write(dir / "models" / "set" / "big-00002-of-00002.gguf", "");
    write(dir / "models" / "set" / "deeper" / "hidden.gguf", "");
    write(dir / "outside" / "far.gguf", "");
    fs::create_directory_symlink(dir / "outside", dir / "models" / "linked", ec);
    const bool linked = !ec;
    const std::vector<ModelFiles> scanned = scan_model_files(models.front());
    const auto listed = [&](const std::string & folder, const std::string & first) {
        for (const ModelFiles & m : scanned)
            if (m.folder == folder && m.first == first) return &m;
        return (const ModelFiles *) nullptr;
    };
    check(listed("", "a.gguf") != nullptr, "scan: a model in the folder itself is listed");
    const ModelFiles * set = listed("set", "big-00001-of-00002.gguf");
    check(set && set->complete() && set->files.size() == 2, "scan: a split set in a subfolder is one model, complete");
    check(!listed("set/deeper", "hidden.gguf") && !listed("deeper", "hidden.gguf"),
          "scan: a folder inside a subfolder is not entered");
    if (linked) {
        check(!listed("linked", "far.gguf"), "scan: a link to a folder is not followed");
        check(!confine_model_path("linked/far.gguf", models, out, err) &&
                  !confine_model_path((dir / "models" / "linked" / "far.gguf").u8string(), models, out, err),
              "scan: and a model through it, named by hand, is refused by the confinement");
    }
    check(confine_model_path("set/big-00001-of-00002.gguf", models, out, err) &&
              fs::equivalent(fs::u8path(out), dir / "models" / "set" / "big-00001-of-00002.gguf"),
          "scan: a model in a subfolder opens through the confinement");
    check(!confine_model_path("set/../../secret.gguf", models, out, err) &&
              !confine_model_path((dir / "models" / "set" / ".." / ".." / "secret.gguf").u8string(), models, out, err),
          "scan: a way out through the subfolder is refused");
    fs::remove_all(dir, ec);
}

static void decide_tests() {
    DecideRequest r;
    std::string err;
    check(parse_decide_request(json::parse(R"({"prefix":"p","suffix":"s","choices":["A","B"]})"), r, err) &&
              r.prefix == "p" && r.suffix == "s" && r.choices.size() == 2 && r.choices[1] == "B" && r.reuse_prefix,
          "a decide body is read whole, reuse_prefix defaulting to true");
    check(parse_decide_request(json::parse(R"({"choices":["A"],"reuse_prefix":false})"), r, err) && r.prefix.empty() &&
              !r.reuse_prefix,
          "prefix and suffix may be left out; reuse_prefix may be turned off");
    check(!parse_decide_request(json::parse(R"({"prefix":"p"})"), r, err) && err.find("choices") != std::string::npos,
          "no choices is refused");
    check(!parse_decide_request(json::parse(R"({"choices":["A",1]})"), r, err),
          "a choice that is not a string is refused");
    check(!parse_decide_request(json::parse(R"({"choices":["A"],"prefix":3})"), r, err),
          "a prefix that is not a string is refused");
    check(!parse_decide_request(json::parse(R"({"choices":["A"],"reuse_prefix":"yes"})"), r, err),
          "reuse_prefix must be a boolean");
    check(!parse_decide_request(json::parse(R"([1])"), r, err), "a body that is not an object is refused");

    DecideResult d;
    d.ok = true;
    d.best = 1;
    d.choice_logp = {-3.25, -0.5, -std::numeric_limits<double>::infinity()};
    d.n_tokens = 10;
    d.n_reused = 4;
    d.n_prefilled = 6;
    d.prefill.seconds = 0.5;
    d.prefix_state_bytes = 2 * 1024 * 1024;
    const json j = decide_json(d);
    check(j["best"] == 1 && j["choice_logp"].size() == 3 && j["choice_logp"][0] == -3.25 &&
              j["choice_logp"][2].is_null(),
          "the answer carries best and the log-probabilities, null for a choice with no mass");
    check(j["n_tokens"] == 10 && j["n_reused"] == 4 && j["n_prefilled"] == 6 && j["prefill_s"] == 0.5 &&
              j["prefix_state_mib"] == 2.0 && j["cancelled"] == false,
          "the counts, the prefill figures and the kept state travel with it");
}

static void model_files_tests() {
    ShardName sh;
    check(parse_shard_name("Qwen3-235B-A22B-Q2_K-00002-of-00005.gguf", sh) && sh.stem == "Qwen3-235B-A22B-Q2_K" &&
              sh.index == 2 && sh.count == 5,
          "a gguf-split name parses into stem, index and count");
    check(parse_shard_name("m-00001-OF-00002.GGUF", sh) && sh.stem == "m" && sh.index == 1 && sh.count == 2,
          "the separator and the extension match in any case");
    check(!parse_shard_name("model.gguf", sh) && !parse_shard_name("-00001-of-00002.gguf", sh) &&
              !parse_shard_name("m-0001-of-00002.gguf", sh) && !parse_shard_name("m-00001-of-0002.gguf", sh) &&
              !parse_shard_name("m-000a1-of-00002.gguf", sh) && !parse_shard_name("m-00001_of-00002.gguf", sh) &&
              !parse_shard_name("m-00001-of-00002.gguf.part", sh),
          "anything else is not a shard: no stem, a digit short, a letter, another separator, a partial file");
    check(!parse_shard_name("m-00000-of-00002.gguf", sh) && !parse_shard_name("m-00003-of-00002.gguf", sh) &&
              !parse_shard_name("m-00001-of-00000.gguf", sh),
          "an index of 0, past the count, or a count of 0 cannot be a shard");
    check(shard_file_name("m", 1, 12) == "m-00001-of-00012.gguf", "a shard's name, as gguf-split writes it");
    check(is_gguf_name("a.GGUF") && !is_gguf_name("a.gguf.part") && !is_gguf_name("gguf"), "what a gguf file is");

    const std::vector<ModelFiles> g =
        group_model_files({"b-00002-of-00003.gguf", "a.gguf", "notes.txt", "b-00001-of-00003.gguf",
                           "c-00002-of-00002.gguf", "a.gguf.part"});
    check(g.size() == 3, "three models: a file, a set, and a set without its first shard");
    check(g.size() == 3 && g[0].first == "a.gguf" && g[0].count == 1 && g[0].complete(), "a single file is complete");
    check(g.size() == 3 && g[1].first == "b-00001-of-00003.gguf" && g[1].count == 3 && !g[1].complete() &&
              g[1].files == std::vector<std::string>{"b-00001-of-00003.gguf", "b-00002-of-00003.gguf"},
          "a set is listed by its first shard, with the files present in shard order");
    check(g.size() == 3 && g[2].first == "c-00001-of-00002.gguf" && g[2].files.size() == 1 && !g[2].complete(),
          "a set missing its first shard is still listed under the first shard's name, incomplete");

    // What a file is for, from its header. The facts mirror three companion files of one released
    // repository and a model that carries its own prediction head.
    GgufHeaderFacts h;
    check(file_role(h) == FileRole::Unknown && std::string(file_role_name(file_role(h))).empty(),
          "role: an unreadable header says nothing");
    h.ok = true;
    h.arch = "clip";
    h.type = "mmproj";
    check(file_role(h) == FileRole::Projector, "role: a projector says so in general.type");
    h.type.clear();
    check(file_role(h) == FileRole::Projector, "role: and an older one by its projector architecture");
    h = {};
    h.ok = true;
    h.arch = "dflash";
    h.type = "model";
    h.n_block = 5;
    h.reads_target = true;
    check(file_role(h) == FileRole::Draft, "role: a file that reads another model's layers is a draft");
    h.arch = "gemma4-assistant";
    h.reads_target = false;
    h.n_block = 4;
    h.n_nextn = 4;
    check(file_role(h) == FileRole::Draft, "role: a file of prediction heads and nothing else is a draft");
    h.arch = "glm4moe";
    h.n_block = 47;
    h.n_nextn = 1;
    check(file_role(h) == FileRole::Model, "role: a model with its own prediction head is a model");
    h.type.clear();
    check(file_role(h) == FileRole::Model, "role: and so is one with no general.type");
    h.n_block = 0;
    h.n_nextn = 0;
    check(file_role(h) == FileRole::Other, "role: no transformer blocks is no language model");
    h.n_block = 10;
    h.type = "adapter";
    check(file_role(h) == FileRole::Other && std::string(file_role_name(FileRole::Other)) == "other",
          "role: another file type is not a model");
    check(file_role_hint("mmproj-gemma-4-26B-A4B-it-Q8_0.gguf") == FileRole::Projector &&
              file_role_hint("sub/Model-MMPROJ-f16.gguf") == FileRole::Projector &&
              file_role_hint("mtp-gemma-4-26B-A4B-it-Q8_0.gguf") == FileRole::Draft &&
              file_role_hint("dflash-x-BF16.gguf") == FileRole::Draft &&
              file_role_hint("gemma-4-26B-A4B-it-Q4_0.gguf") == FileRole::Model,
          "role: a name only hints, for ordering a listing whose headers are unread");
}

// A repository as a person types it, what a listing holds, and the catalog entry a model becomes.
static void hf_tests() {
    HfRef r;
    std::string err;
    check(parse_hf_ref("ggml-org/gemma-4-26B-A4B-it-GGUF", r, err) && r.repo == "ggml-org/gemma-4-26B-A4B-it-GGUF" &&
              r.revision == "main" && r.file.empty(),
          "hf: an id is owner/name on main");
    check(parse_hf_ref("  https://huggingface.co/unsloth/Qwen3.8-Flash-Next-GGUF/tree/main/UD-IQ3_XXS ", r, err) &&
              r.repo == "unsloth/Qwen3.8-Flash-Next-GGUF" && r.revision == "main" && r.file.empty(),
          "hf: a tree URL names the repository and no file");
    check(parse_hf_ref("https://hf.co/a/b/resolve/v2/sub/model-Q4_0.gguf?download=true", r, err) && r.repo == "a/b" &&
              r.revision == "v2" && r.file == "sub/model-Q4_0.gguf",
          "hf: a resolve URL names the file, the revision, and drops the query");
    check(!parse_hf_ref("https://example.com/a/b", r, err), "hf: another host is refused");
    check(!parse_hf_ref("onlyowner", r, err), "hf: one segment is refused");
    check(!parse_hf_ref("a/b/blob/main/../x.gguf", r, err), "hf: a path that climbs is refused");
    check(!parse_hf_ref("a/b c", r, err), "hf: a space in the id is refused");
    check(!parse_hf_ref("a/b/resolve/main/x%2F..%2Fy.gguf", r, err), "hf: an escaped path is refused");

    check(quant_from_name("Qwen3.6-35B-A3B-Q4_K_M.gguf") == "Q4_K_M", "hf: quant Q4_K_M");
    check(quant_from_name("UD-IQ3_XXS/Qwen3.8-Flash-Next-UD-IQ3_XXS-00001-of-00003.gguf") == "UD-IQ3_XXS",
          "hf: quant with the unsloth prefix, off a shard name");
    check(quant_from_name("gemma-4-26B-A4B-it-BF16.gguf") == "BF16", "hf: quant BF16");
    check(quant_from_name("mmproj-gemma-4-26B-A4B-it.gguf").empty(), "hf: no quant token is empty");

    const json tree = json::parse(R"([
      {"type":"file","path":"README.md","size":467},
      {"type":"file","path":"Qwen3.6-35B-A3B-Q4_K_M.gguf","size":20419565568,
       "lfs":{"oid":"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08","size":20419565568}},
      {"type":"file","path":"mmproj-Qwen3.6-35B-A3B-Q8_0.gguf","size":614194304},
      {"type":"file","path":"mtp-Qwen3.6-35B-A3B-Q8_0.gguf","size":4194304},
      {"type":"file","path":"Q2_K/big-Q2_K-00002-of-00003.gguf","size":200},
      {"type":"file","path":"Q2_K/big-Q2_K-00001-of-00003.gguf","size":100},
      {"type":"directory","path":"Q2_K"}
    ])");
    std::vector<HfModel> models;
    check(parse_hf_tree(tree, models, err) && models.size() == 4, "hf: a listing's gguf files, shards grouped");
    check(models.size() == 4 && models[0].role == "model" && models[1].role == "model" && models[2].role == "draft" &&
              models[3].role == "projector",
          "hf: what the names suggest are models come first, the companion files after them");
    check(models[0].name == "Q2_K/big-Q2_K-00001-of-00003.gguf" && models[0].count == 3 &&
              models[0].files.size() == 2 && !models[0].complete() && models[0].bytes == 300 &&
              models[0].files[0].bytes == 100,
          "hf: a split set is listed by its first shard, in shard order, with the shards it has");
    check(models[1].name == "Qwen3.6-35B-A3B-Q4_K_M.gguf" && models[1].quant == "Q4_K_M" && models[1].complete() &&
              models[1].bytes == 20419565568ull,
          "hf: a lone file is a complete model of one");
    std::vector<HfModel> none;
    check(!parse_hf_tree(json{{"error", "Repository not found"}}, none, err) && err == "Repository not found" &&
              none.empty(),
          "hf: the site's error object is the reason, and the listing is empty");

    check(hf_tree_url("https://huggingface.co", r) == "https://huggingface.co/api/models/a/b/tree/v2?recursive=true",
          "hf: the listing URL");
    check(hf_search_url("https://huggingface.co", "gemma 4 26B/it", 20) ==
              "https://huggingface.co/api/"
              "models?search=gemma%204%2026B%2Fit&filter=gguf&sort=downloads&direction=-1&limit=20"
              "&expand%5B%5D=downloads&expand%5B%5D=likes&expand%5B%5D=lastModified&expand%5B%5D=gated",
          "hf: the search URL, the query percent-encoded and every field a hit carries asked for");
    std::vector<HfRepoHit> hits;
    check(parse_hf_search(json::parse(R"([
            {"id":"unsloth/gemma-4-26B-A4B-it-GGUF","downloads":886490,"likes":1224,"lastModified":"2026-10-01T00:00:00.000Z","gated":false},
            {"id":"google/gemma-4-26B-A4B-it-qat-q4_0-gguf","downloads":229421,"likes":194,"gated":"auto"},
            {"id":"bad id with spaces","downloads":1},
            {"downloads":5}
          ])"),
                          hits, err) &&
              hits.size() == 2 && hits[0].id == "unsloth/gemma-4-26B-A4B-it-GGUF" && hits[0].downloads == 886490 &&
              hits[0].likes == 1224 && !hits[0].gated && hits[1].gated && hits[0].modified.rfind("2026-10-01", 0) == 0,
          "hf: a search answer's repositories, a gated one marked, an invalid id and a nameless entry dropped");
    check(!parse_hf_search(json{{"error", "Bad request"}}, hits, err) && err == "Bad request",
          "hf: the site's search error");
    HfRef g;
    parse_hf_ref("ggml-org/gemma-4-26B-A4B-it-GGUF", g, err);
    const CatalogEntry e = hf_catalog_entry("https://huggingface.co", g, models[1]);
    check(e.id == "hf:ggml-org/gemma-4-26B-A4B-it-GGUF/Qwen3.6-35B-A3B-Q4_K_M.gguf" &&
              e.title == "Qwen3.6-35B-A3B-Q4_K_M" && e.quant == "Q4_K_M" && e.files.size() == 1 &&
              e.files[0].name == "Qwen3.6-35B-A3B-Q4_K_M.gguf" &&
              e.files[0].url == "https://huggingface.co/ggml-org/gemma-4-26B-A4B-it-GGUF/resolve/main/"
                                "Qwen3.6-35B-A3B-Q4_K_M.gguf?download=true" &&
              e.files[0].bytes == 20419565568ull && is_hf_catalog_id(e.id) &&
              e.files[0].sha256 == "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
          "hf: the catalog entry a model becomes, with the LFS oid as the file's digest");
    check(models[0].files[0].sha256.empty(), "hf: a file the listing gives no oid for has no digest");
    const CatalogEntry split = hf_catalog_entry("https://mirror.example", g, models[0]);
    check(split.files.size() == 2 && split.files[1].name == "big-Q2_K-00002-of-00003.gguf" &&
              split.files[1].url == "https://mirror.example/ggml-org/gemma-4-26B-A4B-it-GGUF/resolve/main/Q2_K/"
                                    "big-Q2_K-00002-of-00003.gguf?download=true",
          "hf: shards keep their names and their folder in the URL, on the endpoint given");

    FitInputs in;
    in.file_bytes = 10ull << 30;
    in.disk_free = 4ull << 30;
    in.readable = true;
    in.is_moe = true;
    in.streamable = true;
    in.dense_bytes = 1ull << 30;
    in.token_cycle = 512ull << 20;
    in.budget = 6ull << 30;
    FitVerdict v = fit_verdict(in);
    check(!v.downloadable && v.disk.find("6144 MiB short") != std::string::npos && v.mode == "streams",
          "hf: a disk too small is the one thing that refuses the download, and says by how much");
    in.disk_free = 40ull << 30;
    in.present_bytes = 2ull << 30;
    v = fit_verdict(in);
    check(v.downloadable && v.disk.find("needs 8192 MiB") != std::string::npos &&
              v.memory.find("fits in the 6144 MiB") != std::string::npos,
          "hf: what is already on disk is not downloaded again, and the floor against the budget");
    in.budget = 1ull << 30;
    check(fit_verdict(in).memory.find("more than the 1024 MiB") != std::string::npos,
          "hf: a floor past the budget says so");
    in.is_moe = false;
    in.streamable = false;
    check(fit_verdict(in).mode == "plain mmap", "hf: a dense model runs as plain mmap");
    in.is_moe = true;
    in.refusal = "layer 1 names {ffn_up_exps} where layer 0 names more";
    v = fit_verdict(in);
    check(v.mode == "plain mmap" && v.memory.find(in.refusal) != std::string::npos,
          "hf: a refused MoE carries the reason");
    in.readable = false;
    check(fit_verdict(in).mode == "not readable", "hf: an unreadable header is a fact, not a guess");

    in.readable = true;
    in.refusal.clear();
    in.role = "projector";
    v = fit_verdict(in);
    check(v.mode == "not a model" && !v.downloadable && v.memory.find("projector") != std::string::npos,
          "hf: a projector is not a model, and is not offered for download");
    in.role = "draft";
    check(fit_verdict(in).mode == "not a model" && fit_verdict(in).memory.find("draft") != std::string::npos,
          "hf: nor is a draft head");
    in.role = "other";
    check(fit_verdict(in).mode == "not a model", "hf: nor a file with no transformer blocks");

    // The model loaded now is given back: loading this one unloads it first (#285). The figures are
    // the measured case: 450 MiB free with a session holding ~8.6 GB, 9323 MiB once it was unloaded.
    in.role = "model";
    in.is_moe = true;
    in.streamable = true;
    in.disk_free = 40ull << 30;
    in.file_bytes = 14ull << 30;
    in.dense_bytes = 2ull << 30;
    in.token_cycle = 600ull << 20;
    in.budget = 450ull << 20;
    v = fit_verdict(in);
    check(v.memory.find("more than the 450 MiB") != std::string::npos && v.budget == (450ull << 20),
          "hf: with nothing loaded, the budget is what is free now");
    in.session_model = "Qwen3.6-35B-A3B-Q4_K_M.gguf";
    v = fit_verdict(in);
    check(v.budget == (450ull << 20) &&
              v.memory.find("measured with Qwen3.6-35B-A3B-Q4_K_M.gguf loaded") != std::string::npos,
          "hf: a session with no ledger cannot be given back, and the verdict says the figure is with it loaded");
    in.session_held = 8873ull << 20;
    v = fit_verdict(in);
    check(v.budget == (9323ull << 20) && v.memory.find("fits in the 9323 MiB") != std::string::npos &&
              v.memory.find("8873 MiB of it held now by Qwen3.6-35B-A3B-Q4_K_M.gguf") != std::string::npos,
          "hf: the session's ledger is given back, and the verdict says how much and by whom");
    in.memory_total = 8ull << 30;
    check(fit_verdict(in).budget == (8ull << 30), "hf: never more than the machine has");
    in.is_moe = false;
    in.streamable = false;
    check(fit_verdict(in).memory.find("held now by") != std::string::npos,
          "hf: a dense model's verdict is given the same budget");

    // The verdict's code and headline, which a UI shows and colours by without a threshold of its own.
    FitInputs f;
    f.file_bytes = 10ull << 30;
    f.disk_free = 40ull << 30;
    f.readable = f.is_moe = f.streamable = true;
    f.dense_bytes = 1ull << 30;
    f.token_cycle = 512ull << 20;
    f.budget = 6ull << 30;
    check(fit_verdict(f).fit == "fits" && !fit_verdict(f).headline.empty(), "hf: a floor under the budget fits");
    f.budget = 1ull << 30;
    check(fit_verdict(f).fit == "will_not_fit", "hf: a floor past the budget will not fit");
    f.budget = 0;
    check(fit_verdict(f).fit == "unknown" && fit_verdict(f).headline.find("could not be read") != std::string::npos,
          "hf: an unknown budget is no fit claimed: the code is unknown, the headline says why");
    f.streamable = false;
    check(fit_verdict(f).fit == "no_streaming", "hf: a MoE this build does not stream runs without streaming");
    f.is_moe = false;
    check(fit_verdict(f).fit == "no_streaming", "hf: so does a dense model");
    f.disk_free = 1ull << 30;
    check(fit_verdict(f).fit == "no_disk" && !fit_verdict(f).downloadable, "hf: a disk too small comes first");
    f.role = "projector";
    check(fit_verdict(f).fit == "not_a_model", "hf: but a file that is no model says that first");
    f.readable = false;
    check(fit_verdict(f).fit == "no_disk", "hf: an unreadable file on a full disk is still refused for the disk");
    f.disk_free = 40ull << 30;
    check(fit_verdict(f).fit == "unknown", "hf: and with room, it is unknown");
}

// A synthetic machine and plan: the view is pure, so it is driven with the facts alone.
static ComputeDevice device(const char * name, bool cpu = false) {
    ComputeDevice d;
    d.name = name;
    d.is_cpu = cpu;
    return d;
}

static const json * device_named(const json & view, const std::string & name) {
    for (const json & d : view["devices"])
        if (d.value("name", "") == name) return &d;
    return nullptr;
}

static void plan_view_tests() {
    {
        // Which held plan answers a read with a model loaded: the load's own while nothing was
        // measured after it; the last measured for the same model only when a held one was asked
        // for; nothing for another model, or with nothing held.
        PlanRead r;
        r.model = "/m/a.gguf";
        check(held_plan(r) == HeldPlan::None, "held plan: nothing held, nothing answers");
        r.load_model = "/m/a.gguf";
        r.load_seq = 1;
        r.last_model = "/m/a.gguf";
        r.last_seq = 1;
        check(held_plan(r) == HeldPlan::Runs, "held plan: the load's plan, which is the last measured");
        r.want_cached = true;
        check(held_plan(r) == HeldPlan::Runs, "held plan: the same when a held one is asked for");
        r.last_seq = 2;
        check(held_plan(r) == HeldPlan::Last, "held plan: a measurement after the load is the last one");
        r.want_cached = false;
        check(held_plan(r) == HeldPlan::None, "held plan: ...but only when a held one was asked for");
        r.want_cached = true;
        r.last_model = "/m/b.gguf";
        check(held_plan(r) == HeldPlan::None, "held plan: a plan for another model does not answer");
        r.last_seq = 0;
        r.last_model.clear();
        check(held_plan(r) == HeldPlan::Runs, "held plan: the load's plan with no measurement held");
        r.model = "/m/b.gguf";
        check(held_plan(r) == HeldPlan::None, "held plan: the load's plan for another model does not");
    }
    // A phone: the NPU is armed for the prompt, a GPU is slower, another gave the wrong results,
    // one did not survive its probe, and the device slots take turns with the cache.
    HardwareProfile hw;
    hw.backends_looked = true;
    hw.backends_placeable = 3;
    hw.host_wide_matmul_gibs = 30;
    hw.host_bandwidth_gibs = 30;
    hw.devices.push_back(device("CPU", true));
    ComputeDevice npu = device("NPU0");
    npu.wide_matmul_gibs = 1200;
    npu.wide_identity_ok = Tri::Yes;
    hw.devices.push_back(npu);
    ComputeDevice gpu = device("GPU0");
    gpu.wide_matmul_gibs = 180;
    gpu.wide_identity_ok = Tri::Yes;
    hw.devices.push_back(gpu);
    ComputeDevice wrong = device("GPU1");
    wrong.wide_matmul_gibs = 300;
    wrong.wide_identity_ok = Tri::No;
    hw.devices.push_back(wrong);
    ComputeDevice dead = device("GPU2");
    dead.usable = Tri::No;
    dead.unusable_reason = "the probe ended without an answer";
    hw.devices.push_back(dead);
    Plan plan;
    plan.regime = Regime::ExpertsStream;
    plan.config.prefill.device = "NPU0";
    plan.allocation.cache_bytes = 2ull << 30;
    plan.allocation.seconds_per_token = 0.3;
    plan.allocation.seconds_worst_case = 1.2;
    plan.allocation.cache_hit_credited = 0.2;
    plan.allocation.ledger.holdable_cap = 5ull << 30;
    plan.allocation.ledger.rows = {{"dense", 1ull << 30, true, false},
                                   {"expert cache", 2ull << 30, true, false},
                                   {"device slots", 1ull << 29, true, true}};
    json v = plan_view(hw, plan);
    check(v["summary"]["sentence"] == "Experts stream from flash, with a 2.0 GiB cache.", "plan view: the sentence");
    check(v["summary"]["lanes"][0]["on"] == json::array({"NPU0"}) && v["summary"]["lanes"][0]["label"] == "Prompt",
          "plan view: the prompt lane names the armed device");
    check(v["summary"]["lanes"][1]["on"] == json::array({"CPU"}), "plan view: tokens stay on the host");
    check(v["predicted"]["s_per_token"] == 0.3 && v["predicted"]["cache_hit_pct"] == 20.0 &&
              v["predicted"]["s_per_token_worst"] == 1.2,
          "plan view: the prediction the CLI prints, its credited hits and its bound");
    const json * c = device_named(v, "CPU");
    check(c && (*c)["uses"] == json::array({"tokens"}) && (*c)["badge"] == "Tokens" &&
              (*c)["rate_text"] == "The reference",
          "plan view: the host decodes, the prompt went to a device");
    const json * n = device_named(v, "NPU0");
    check(n && (*n)["status"] == "used" && (*n)["badge"] == "Prompt" && (*n)["rate_text"] == "40x the host on a prompt",
          "plan view: the armed device, with its rate");
    const json * g = device_named(v, "GPU0");
    check(g && (*g)["status"] == "unused" && (*g)["badge"] == "Not used" &&
              (*g)["why"].get<std::string>().find("NPU0") != std::string::npos,
          "plan view: a slower device says which one was armed");
    const json * w = device_named(v, "GPU1");
    check(w && (*w)["status"] == "refused" && (*w)["badge"] == "Wrong results", "plan view: wrong results refuse");
    const json * d = device_named(v, "GPU2");
    check(d && (*d)["status"] == "unusable" && (*d)["why"] == "the probe ended without an answer",
          "plan view: a device that died says why");
    check(v["devices_none"] == "" && v["devices_none_text"] == "", "plan view: devices, so no none line");
    const json & pools = v["memory"]["pools"];
    check(pools.size() == 1 && pools[0]["label"] == "This process" && pools[0]["rows"].size() == 3 &&
              pools[0]["rows"][1]["label"] == "Expert cache" && pools[0]["rows"][2]["in_turn"] == true,
          "plan view: one pool, the ledger rows worded, the slots in turn");
    check(pools[0]["lockable_cap_known"] == true && pools[0]["cache_lock_unverified"] == false,
          "plan view: a measured lock ceiling retains its known status");
    Plan unknown_lock = plan;
    unknown_lock.allocation.ledger.lockable_cap_known = false;
    unknown_lock.allocation.cache_lock_unverified = true;
    const json uncertain_memory = plan_view(hw, unknown_lock)["memory"]["pools"][0];
    check(uncertain_memory["lockable_cap_known"] == false && uncertain_memory["cache_lock_unverified"] == true &&
              uncertain_memory["cap_bytes"] == pools[0]["cap_bytes"],
          "plan view: an unmeasured lock ceiling and pending grant stay distinct from process capacity");

    // The same view on a desktop whose card holds layers in memory of its own: a second pool, and
    // the card on the tokens lane beside the host.
    HardwareProfile desk;
    desk.backends_looked = true;
    desk.devices.push_back(device("CPU", true));
    ComputeDevice card = device("CUDA0");
    card.shares_host_memory = Tri::No;
    card.memory_total = 12ull << 30;
    desk.devices.push_back(card);
    Plan dp;
    dp.regime = Regime::ExpertsStream;
    dp.placement.fitted = true;
    dp.placement.device_bytes = 9ull << 30;
    dp.placement.device_compute_bytes = 1ull << 30;
    dp.placement.projected_devices = {"CUDA0"};
    dp.placement.projected_compute_bytes = {1ull << 30};
    v = plan_view(desk, dp);
    check(v["memory"]["pools"].size() == 2 && v["memory"]["pools"][1]["name"] == "CUDA0" &&
              v["memory"]["pools"][1]["rows"][0]["bytes"] == (8ull << 30) &&
              v["memory"]["pools"][1]["estimated"] == false,
          "plan view: a device's own memory is its own pool");
    check(v["summary"]["lanes"][1]["on"] == json::array({"CPU", "CUDA0"}), "plan view: layers put the card on tokens");
    check((*device_named(v, "CUDA0"))["badge"] == "Layers", "plan view: the card's badge");
    dp.placement.shared_memory_placement = true;
    v = plan_view(desk, dp);
    check(v["memory"]["pools"].size() == 1, "plan view: a declined placement holds nothing on the device");

    // The cores alone: three answers to "no device", kept apart.
    HardwareProfile cpu;
    cpu.devices.push_back(device("CPU", true));
    check(plan_view(cpu, Plan{})["devices_none"] == "not_probed", "plan view: nobody looked");
    cpu.backends_looked = true;
    check(plan_view(cpu, Plan{})["devices_none"] == "build", "plan view: the build carries no backend");
    cpu.backends_placeable = 1;
    json cv = plan_view(cpu, Plan{});
    check(cv["devices_none"] == "machine" && !cv["devices_none_text"].get<std::string>().empty(),
          "plan view: a backend found nothing here");
    check(plan_view(cpu, Plan{})["summary"]["sentence"] == "The model could not be read.",
          "plan view: an unread model says so");

    // Decisions: who decided, and what.
    Decision dec{"cache-mb", "2048", Source::Measured, "r"};
    json dj = decision_json(dec);
    check(dj["group"] == "set" && !dj["label"].get<std::string>().empty(), "plan view: a decided setting, labelled");
    dec.source = Source::Operator;
    check(decision_json(dec)["group"] == "kept", "plan view: the person's value is kept");
    dec.source = Source::Unprobed;
    check(decision_json(dec)["group"] == "kept", "plan view: an unmeasured value is kept");
    check(value_text(Decision{"cache-mb", "4737", Source::Derived, "r", "b"}) == "4.6 GiB" &&
              value_text(Decision{"dense-weights", "pinned", Source::Derived, "r", "b"}) == "pinned in memory" &&
              value_text(Decision{"o-direct", "true", Source::Measured, "r", "b"}) == "on" &&
              value_text(Decision{"memory-ledger", "7265 MiB", Source::Derived, "r", "b"}) == "7.1 GiB" &&
              value_text(Decision{"regime", "experts-stream", Source::Derived, "r", "b"}) == "experts stream" &&
              value_text(Decision{"shared-memory", "-", Source::Derived, "r", "b"}).empty(),
          "plan view: a value as a person reads it");
    check(decision_json(Decision{"storage-rate", "6.3 GiB/s", Source::Derived, "r", "b"})["group"] == "notes",
          "plan view: a cost model's note is a note");
    check(decision_json(Decision{"regime", "experts-stream", Source::Derived, "r"})["label"] == "Streaming mode",
          "plan view: a note is titled by its topic");
    check(decision_json(Decision{"some-new-topic", "-", Source::Derived, "r"})["label"] == "Some new topic",
          "plan view: a topic it does not know is titled from its key");
    check(decision_json(Decision{"regime", "-", Source::Derived, "the reason"})["brief"] == "the reason",
          "plan view: a decision without a brief shows its reason");

    // A real plan, through the wire form: every line says itself in one sentence, and every note has a
    // name to be read under.
    {
        ModelProfile m;
        m.arch = "qwen3moe";
        m.is_moe = m.ok = true;
        m.n_layer = m.n_moe_layer = 48;
        m.n_expert = 256;
        m.n_expert_used = 8;
        m.n_expert_projections = 3;
        m.expert_slice_bytes = 530432;
        m.token_cycle_bytes = 48ull * 8 * 3 * 530432;
        m.file_bytes = 21ull << 30;
        m.expert_bytes = 18ull << 30;
        m.dense_bytes = 3ull << 30;
        m.largest_dense_tensor = 243ull << 20;
        m.group(WeightGroup::Attention) = {1400ull << 20, 1400ull << 20, false, false};
        m.group(WeightGroup::Output) = {700ull << 20, 700ull << 20, false, false};
        m.group(WeightGroup::Experts) = {18ull << 30, m.token_cycle_bytes, false, true};
        HardwareProfile h;
        h.residency_budget = 12ull << 30;
        h.memory_total = 16ull << 30;
        h.anon_overflow = Overflow::Compress;
        h.host_bandwidth_gibs = 60;
        h.storage.direct_ok = Tri::Yes;
        h.storage.refault_mibs = 40;
        h.storage.rate_curve = {{576u << 10, 1, 1600.0}, {576u << 10, 4, 6400.0}};
        h.devices.push_back(device("CPU", true));
        ComputeDevice gpu = device("GPU0");
        gpu.shares_host_memory = Tri::Yes;
        h.devices.push_back(gpu);
        const Plan p = plan_run(RunConfig{}, h, m, PlanRequest{});
        bool briefs = !p.decisions.empty(), labels = true, any_note = false;
        std::string bad;
        for (const Decision & d : p.decisions) {
            const json j = decision_json(d);
            const std::string brief = j["brief"];
            if (d.brief.empty() || brief.size() > 120) briefs = false, bad += d.knob + " ";
            if (j["group"] == "notes") {
                any_note = true;
                if (j["label"].get<std::string>().empty()) labels = false, bad += d.knob + " ";
            }
        }
        check(briefs, "plan view: every decision of a plan has a brief of one line" + (bad.empty() ? "" : ": " + bad));
        check(any_note && labels, "plan view: every note of a plan has a label" + (bad.empty() ? "" : ": " + bad));
    }
    check(plan_step_label("rules") == "Decisions" && plan_step_label("future") == "Future",
          "plan view: stage words, and a stage it does not know");
}

static void download_tests() {
    namespace fs = std::filesystem;
    check(is_transient_curl_exit(18) && is_transient_curl_exit(56) && is_transient_curl_exit(7) &&
              is_transient_curl_exit(28),
          "downloads: a short body, a reset, a refused connection and a stall are retried");
    check(!is_transient_curl_exit(0) && !is_transient_curl_exit(22) && !is_transient_curl_exit(23) &&
              !is_transient_curl_exit(33),
          "downloads: an HTTP error, a write error (full disk) and a refused range are not");
    check(download_retry_delay(1) == kDownloadRetryFirstDelay &&
              download_retry_delay(2) == 2 * kDownloadRetryFirstDelay,
          "downloads: the wait doubles after each failed attempt");
    check(download_retry_delay(1000) == kDownloadRetryMaxDelay, "downloads: and stops at its cap");
    bool rising = true;
    for (int i = 2; i <= kDownloadMaxAttempts; ++i)
        rising = rising && download_retry_delay(i) >= download_retry_delay(i - 1);
    check(rising, "downloads: never shorter than the one before");

    check(bytes_still_needed(47, 33) == 14 && bytes_still_needed(10, 12) == 0,
          "downloads: a resume needs only what the partial file lacks");
    check(free_space_refusal(14'000'000'000ull, 20'000'000'000ull).empty(),
          "downloads: 14 GB still to fetch fits in 20 GB free, whatever the whole model weighs");
    const std::string no = free_space_refusal(14'000'000'000ull, 3'000'000'000ull);
    check(no == "needs 14.0 GB more, only 3.0 GB free", "downloads: and a disk too small says by how much: " + no);
    check(free_space_refusal(14'000'000'000ull, 0).empty(), "downloads: unknown free space refuses nothing");

    const fs::path dir = fs::temp_directory_path() / ("bmoe-dl-test-" + std::to_string(std::rand()));
    const fs::path models = dir / "models";
    fs::create_directories(models);
    const auto write = [](const fs::path & p, const char * s) {
        std::FILE * f = std::fopen(p.u8string().c_str(), "wb");
        if (!f) return;
        std::fputs(s, f);
        std::fclose(f);
    };
    std::string out;
    check(partial_file_path(models.u8string(), "m.gguf", out) && fs::u8path(out) == models / "m.gguf.part",
          "downloads: a file's partial is <name>.part in the models folder");
    for (const char * bad : {"../m.gguf", "a/b.gguf", "a\\b.gguf", "..", ".", "", "C:m.gguf"})
        check(!partial_file_path(models.u8string(), bad, out),
              std::string("downloads: '") + bad + "' names no partial");

    check(is_plain_file_name("m-00001-of-00003.gguf") && !is_plain_file_name("../m.gguf") &&
              !is_plain_file_name("sub/m.gguf") && !is_plain_file_name("..") && !is_plain_file_name("") &&
              !is_plain_file_name(std::string("m\0.gguf", 7)),
          "downloads: a catalog file name is a bare name or nothing");
    const std::string url = "https://huggingface.co/a/b/resolve/main/m.gguf";
    const std::string hf = "https://huggingface.co";
    check(catalog_file_refusal("m.gguf", url, "", hf).empty() &&
              catalog_file_refusal("m.gguf", url, std::string(64, 'a'), hf).empty(),
          "downloads: a plain name with an https URL is offered, with or without a digest");
    check(!catalog_file_refusal("../m.gguf", url, "", hf).empty() &&
              !catalog_file_refusal("C:m.gguf", url, "", hf).empty(),
          "downloads: a catalog row whose name leaves the models folder is refused at load");
    check(!catalog_file_refusal("m.gguf", "-o/etc/passwd", "", hf).empty() &&
              !catalog_file_refusal("m.gguf", "file:///etc/passwd", "", hf).empty(),
          "downloads: a URL that is not http(s), or reads as an option, is refused");
    check(!catalog_file_refusal("m.gguf", url, "abc", hf).empty() &&
              !catalog_file_refusal("m.gguf", url, std::string(64, 'G'), hf).empty(),
          "downloads: a digest that is not 64 hex digits is refused");
    const std::string mirror = "http://mirror.lan";
    check(curl_protocols(url, hf) == "=https" && curl_protocols(url, mirror) == "=https" &&
              curl_protocols("ftp://x", mirror).empty(),
          "downloads: https stays https on every redirect, whatever the endpoint");
    check(curl_protocols("http://mirror.lan/a/b/resolve/main/m.gguf", mirror) == "=http,https",
          "downloads: plain http is let through under the http mirror the operator named");
    check(curl_protocols("http://huggingface.co/a/b/resolve/main/m.gguf", hf).empty() &&
              curl_protocols("http://mirror.lan/x", hf).empty() &&
              curl_protocols("http://mirror.lan.example.com/x", mirror).empty() &&
              curl_protocols("http://mirror.lan@elsewhere/x", mirror).empty() &&
              curl_protocols("http://mirror.lan", mirror).empty(),
          "downloads: any other http URL is refused, a lookalike host or a userinfo trick included");
    check(!catalog_file_refusal("m.gguf", "http://elsewhere/m.gguf", "", mirror).empty() &&
              catalog_file_refusal("m.gguf", "http://mirror.lan/a/b/resolve/main/m.gguf", "", mirror).empty() &&
              !catalog_file_refusal("m.gguf", "http://mirror.lan/a/b/resolve/main/m.gguf", "", hf).empty(),
          "downloads: a catalog row over http is offered only under the http mirror the server was given");

    check(Sha256().hex_digest() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256: the empty input");
    Sha256 abc;
    abc.update("abc", 3);
    check(abc.hex_digest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256: \"abc\"");
    Sha256 two_blocks;
    const std::string nist = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    two_blocks.update(nist.data(), 10);
    two_blocks.update(nist.data() + 10, nist.size() - 10);
    check(two_blocks.hex_digest() == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "sha256: a two-block input fed in pieces");
    Sha256 million;
    const std::string as(1000, 'a');
    for (int i = 0; i < 1000; ++i)
        million.update(as.data(), as.size());
    check(million.hex_digest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "sha256: a million 'a's");

    write(models / "a.gguf.part", "12345");
    write(models / "b.gguf.part", "678");
    write(models / "a.gguf", "whole");
    write(dir / "outside.part", "keep");
    uint64_t freed = 0;
    std::string err;
    check(delete_partial_files(models.u8string(), {"a.gguf", "b.gguf", "absent.gguf"}, freed, err) && freed == 8 &&
              !fs::exists(models / "a.gguf.part") && !fs::exists(models / "b.gguf.part"),
          "downloads: deleting a partial download removes its .part files, a missing one included");
    check(fs::exists(models / "a.gguf"), "downloads: and never the finished file beside them");
    check(!delete_partial_files(models.u8string(), {"../outside"}, freed, err) && !err.empty() &&
              fs::exists(dir / "outside.part"),
          "downloads: a name that walks out of the models folder deletes nothing");
    write(models / "ok.gguf.part", "x");
    check(!delete_partial_files(models.u8string(), {"ok.gguf", "../outside"}, freed, err) &&
              fs::exists(models / "ok.gguf.part") && fs::exists(dir / "outside.part"),
          "downloads: one bad name refuses the whole request before anything is deleted");
    std::error_code ec;
    fs::create_symlink(dir / "outside.part", models / "link.gguf.part", ec);
    if (!ec)
        check(delete_partial_files(models.u8string(), {"link.gguf"}, freed, err) && fs::exists(dir / "outside.part") &&
                  !fs::exists(fs::symlink_status(models / "link.gguf.part")),
              "downloads: a link named like a partial is removed as the link, its target kept");
    write(models / "abc.gguf.part", "abc");
    std::string hex;
    check(sha256_file((models / "abc.gguf.part").u8string(), hex, err) &&
              hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
              digest_refusal(hex, hex).empty() && digest_refusal("", hex).empty(),
          "downloads: a finished file whose digest is the catalog's is accepted");
    check(!digest_refusal(std::string(64, '0'), hex).empty(), "downloads: and one whose digest differs is not");
    {
        // Past the reader's chunk, so its seams are crossed: the file's digest is the one of its bytes.
        std::string big(9u << 20, '\0');
        for (size_t i = 0; i < big.size(); ++i)
            big[i] = (char) (i * 2654435761u >> 13);
        std::FILE * f = std::fopen((models / "big.part").u8string().c_str(), "wb");
        if (f) {
            std::fwrite(big.data(), 1, big.size(), f);
            std::fclose(f);
        }
        Sha256 want;
        want.update(big.data(), 7);
        want.update(big.data() + 7, big.size() - 7);
        check(sha256_file((models / "big.part").u8string(), hex, err) && hex == want.hex_digest(),
              "downloads: a file longer than one read hashes as its bytes do");
    }
    check(!sha256_file((models / "absent").u8string(), hex, err) && !err.empty(),
          "downloads: a file that cannot be read has no digest");
    check(gb_label(47'000'000'000ull) == "47.0 GB", "downloads: sizes are decimal GB, as the Android app writes them");
    fs::remove_all(dir, ec);
}

// The event fan-out: a bounded number of streams, each a worker held for its lifetime.
static void event_bus_tests() {
    EventBus bus(2);
    auto a = bus.subscribe();
    auto b = bus.subscribe();
    check(a && b && !bus.subscribe(), "events: past the cap a stream is refused, not held");
    bus.unsubscribe(a);
    auto c = bus.subscribe();
    check(c != nullptr, "events: a stream that ends gives its place back");
    bus.publish("state", json{{"x", 1}});
    std::string f;
    check(EventBus::next(*b, f, std::chrono::milliseconds(0)) && f.find("event: state") == 0 &&
              EventBus::next(*c, f, std::chrono::milliseconds(0)) && f.find("event: state") == 0,
          "events: every subscriber gets the frame");
    check(EventBus::next(*b, f, std::chrono::milliseconds(0)) && f.empty(), "events: nothing new is an empty frame");
    bus.close_all();
    check(!EventBus::next(*c, f, std::chrono::milliseconds(0)), "events: a closed bus ends every stream");
    check(k_max_event_streams > 0 && EventBus().subscribe() != nullptr, "events: the default bus takes a stream");
}

static void url_entry_tests() {
    UrlModel m;
    std::string err;
    check(parse_model_url("  https://example.org/models/Some-Model-Q4_K_M.gguf?download=true#x ", m, err) &&
              m.files.size() == 1 && m.files[0].name == "Some-Model-Q4_K_M.gguf" &&
              m.files[0].url == "https://example.org/models/Some-Model-Q4_K_M.gguf?download=true" &&
              m.host == "example.org",
          "url: a direct link names one file, its query kept, its fragment and blanks dropped: " + err);
    check(parse_model_url("HTTPS://example.org/a%2Bb.gguf", m, err) && m.files[0].name == "a+b.gguf" &&
              m.files[0].url == "https://example.org/a%2Bb.gguf",
          "url: the scheme in any case, an escape in the name decoded for the file and kept in the link");
    for (const char * bad :
         {"http://example.org/m.gguf", "ftp://example.org/m.gguf", "example.org/m.gguf",
          "https://user:pw@example.org/m.gguf", "https:///m.gguf", "https://example.org", "https://example.org/",
          "https://example.org/m.bin", "https://example.org/m%2F..%2Fx.gguf", "https://example.org/..%2Fm.gguf",
          "https://example.org/.hidden.gguf", "https://example.org/-o.gguf", "https://example.org/a b.gguf",
          "https://example.org/m.gguf\"", "https://example.org/a%2.gguf", "https://example.org/caf%C3%A9.gguf",
          "https://example.org/m.gguf?\x01"})
        check(!parse_model_url(bad, m, err) && !err.empty(), std::string("url: refused: ") + bad);

    check(parse_model_url("https://example.org/r/Big-00002-of-00003.gguf", m, err) && m.files.size() == 3 &&
              m.files[0].name == "Big-00001-of-00003.gguf" && m.files[2].name == "Big-00003-of-00003.gguf" &&
              m.files[1].url == "https://example.org/r/Big-00002-of-00003.gguf",
          "url: a link to any shard names the whole split set, every shard beside it");
    check(!parse_model_url("https://example.org/r/Big%2D00001-of-00002.gguf", m, err),
          "url: a shard link spelled with escapes is refused rather than guessed");
    check(parse_model_url("https://example.org/r/Big-00001-of-00" + std::to_string(kMaxUrlShards) + ".gguf", m, err) &&
              (int) m.files.size() == kMaxUrlShards,
          "url: a set of the most shards a link may name is taken");
    check(!parse_model_url("https://example.org/r/x-00001-of-99999.gguf", m, err) &&
              err.find(std::to_string(kMaxUrlShards)) != std::string::npos,
          "url: a link naming more shards than that is refused before anything is fetched: " + err);

    // The sizes, against a fake host and a fake clock: each answer takes 50 s.
    struct FakeHost : HttpFetch {
        std::chrono::steady_clock::time_point t{};
        int asked = 0;
        int missing = -1; // the index of a file the host says is not there
        bool get(const std::string &, uint64_t, uint64_t, const std::string &, std::string &, std::string &) override {
            return false;
        }
        bool size(const std::string &, uint64_t & bytes, std::string & error) override {
            t += std::chrono::seconds(50);
            if (asked++ == missing) {
                error = "the link answered HTTP 404";
                return false;
            }
            bytes = 1000;
            return true;
        }
    };
    FakeHost host;
    const auto clock = [&host] { return host.t; };
    std::vector<uint64_t> sizes;
    bool timed_out = false;
    parse_model_url("https://example.org/r/Big-00001-of-00002.gguf", m, err);
    check(fetch_url_sizes(host, m, kUrlSizeDeadline, sizes, timed_out, err, clock) && sizes.size() == 2 &&
              sizes[1] == 1000 && !timed_out,
          "url: every shard is asked for its size, in order");
    parse_model_url("https://example.org/r/Big-00001-of-00010.gguf", m, err);
    host = FakeHost{};
    check(!fetch_url_sizes(host, m, kUrlSizeDeadline, sizes, timed_out, err, clock) && timed_out && host.asked < 10 &&
              host.asked * 50 <= kUrlSizeDeadline.count() + 50,
          "url: asking stops at the deadline, and the entry is not made from half its sizes: " + err);
    host = FakeHost{};
    host.missing = 1;
    check(!fetch_url_sizes(host, m, kUrlSizeDeadline, sizes, timed_out, err, clock) && !timed_out && host.asked == 2 &&
              err.find("404") != std::string::npos,
          "url: a shard the host says is not there refuses the link");

    check(parse_model_url("https://example.org/r/Big-00001-of-00002.gguf", m, err), "url: a split set parses");
    const CatalogEntry e = url_catalog_entry(m, {10, 20});
    check(e.id == "url:Big-00001-of-00002.gguf" && e.title == "Big" && e.files.size() == 2 && e.files[0].bytes == 10 &&
              e.files[1].bytes == 20 && is_url_catalog_id(e.id) && !is_url_catalog_id("hf:a/b"),
          "url: the entry is keyed by its first file, titled by the set, sized per file");
    parse_model_url("https://example.org/x-Q8_0.gguf", m, err);
    const CatalogEntry one = url_catalog_entry(m, {});
    check(one.title == "x-Q8_0" && one.quant == "Q8_0" && one.files[0].bytes == 0,
          "url: one file is titled without its extension; a size not given is unknown");

    HeadAnswer a = head_answer("HTTP/1.1 302 Found\r\ncontent-length: 5\r\nlocation: x\r\n\r\n"
                               "HTTP/2 200\r\nContent-Length:  47000000000\r\n\r\n");
    check(a.status == 200 && a.length == 47000000000ull, "url: the size is the last response's, after a redirect");
    a = head_answer("HTTP/1.1 302 Found\r\nContent-Length: 5\r\n\r\nHTTP/2 200\r\n\r\n");
    check(a.status == 200 && a.length == 0, "url: and unknown when the last response has none");
    a = head_answer("HTTP/2 307 \r\ncontent-length: 93\r\n\r\nHTTP/2 404 \r\ncontent-length: 15\r\n\r\n");
    check(a.status == 404, "url: a missing file is the last response's status, whatever curl exits with");
    check(head_answer("HTTP/2 200\r\ncontent-length: 12x\r\n").length == 0, "url: a malformed length is none");
    check(head_answer("").status == 0, "url: no answer at all is no status");

#ifndef _WIN32
    // A server with no curl to ask with (a phone's) makes the entry with the size unknown, as for a
    // host that answers no HEAD: the client that downloads for itself then fetches it. PATH is set
    // to an empty folder for the call, so no curl can be found, and put back after.
    namespace fs = std::filesystem;
    const fs::path empty = fs::temp_directory_path() / ("bmoe-nocurl-" + std::to_string(std::rand()));
    fs::create_directories(empty);
    const char * old_path = std::getenv("PATH");
    const std::string saved = old_path ? old_path : "";
    setenv("PATH", empty.string().c_str(), 1);
    uint64_t bytes = 1;
    std::string why;
    const bool ok = curl_fetch((empty / "scratch").string(), "")->size("https://example.invalid/x.gguf", bytes, why);
    if (old_path)
        setenv("PATH", saved.c_str(), 1);
    else
        unsetenv("PATH");
    std::error_code ec;
    fs::remove_all(empty, ec);
    check(ok && bytes == 0, "url: with no curl on PATH the size is unknown, and the link is not refused");
#endif
}

static void model_delete_tests() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("bmoe-del-test-" + std::to_string(std::rand()));
    const fs::path models = dir / "models";
    fs::create_directories(models / "set");
    const auto write = [](const fs::path & p, const char * s) {
        std::FILE * f = std::fopen(p.u8string().c_str(), "wb");
        if (!f) return;
        std::fputs(s, f);
        std::fclose(f);
    };
    write(models / "one.gguf", "12345");
    write(models / "two.gguf", "12");
    write(models / "set" / "S-00001-of-00002.gguf", "abc");
    write(models / "set" / "S-00002-of-00002.gguf", "defg");
    write(dir / "outside.gguf", "keep");

    ModelDeletion d;
    bool not_found = false;
    std::string err;
    for (const char * bad : {"../outside.gguf", "set/../one.gguf", "a/b/c.gguf", "", "/one.gguf", "..", "C:one.gguf",
                             "set\\S-00001-of-00002.gguf"})
        check(!find_model_to_delete(models.u8string(), bad, d, not_found, err) && !not_found,
              std::string("delete: '") + bad + "' names no model");
    check(!find_model_to_delete(models.u8string(), "absent.gguf", d, not_found, err) && not_found,
          "delete: a plain name that is not there is not found");
    check(!find_model_to_delete(models.u8string(), "set/S-00002-of-00002.gguf", d, not_found, err) && not_found,
          "delete: a split set is named by its first shard, as the listing names it");
    check(find_model_to_delete(models.u8string(), "set/S-00001-of-00002.gguf", d, not_found, err) &&
              d.paths.size() == 2 && d.folder == "set",
          "delete: a split set is every shard the scan grouped: " + err);
    check(deletion_touches(d, (models / "set" / "S-00001-of-00002.gguf").u8string()) &&
              deletion_touches(d, (models / "set" / ".." / "set" / "S-00002-of-00002.gguf").u8string()) &&
              !deletion_touches(d, (models / "one.gguf").u8string()) && !deletion_touches(d, ""),
          "delete: the loaded model is recognised whatever its spelling, and only it");
    uint64_t freed = 0;
    check(delete_model_files(models.u8string(), d, freed, err) && freed == 7 && !fs::exists(models / "set"),
          "delete: every shard goes, and the emptied folder with them");
    check(find_model_to_delete(models.u8string(), "one.gguf", d, not_found, err) && d.paths.size() == 1 &&
              delete_model_files(models.u8string(), d, freed, err) && freed == 5 && !fs::exists(models / "one.gguf") &&
              fs::exists(models / "two.gguf") && fs::exists(dir / "outside.gguf"),
          "delete: one file goes, its neighbours and anything outside stay");
    std::error_code ec;
    fs::create_symlink(dir / "outside.gguf", models / "link.gguf", ec);
    if (!ec)
        check(find_model_to_delete(models.u8string(), "link.gguf", d, not_found, err) &&
                  delete_model_files(models.u8string(), d, freed, err) && fs::exists(dir / "outside.gguf") &&
                  !fs::exists(fs::symlink_status(models / "link.gguf")),
              "delete: a link in the models folder is removed as the link, its target kept");

    // All or nothing: a file that cannot be moved aside (here a directory where a shard should be,
    // as a file held open is on Windows) puts back the ones moved before it and deletes nothing.
    write(models / "A-00001-of-00002.gguf", "abc");
    fs::create_directories(models / "A-00002-of-00002.gguf");
    ModelDeletion half;
    half.names = {"A-00001-of-00002.gguf", "A-00002-of-00002.gguf"};
    half.paths = {(models / half.names[0]).u8string(), (models / half.names[1]).u8string()};
    check(!delete_model_files(models.u8string(), half, freed, err) && freed == 0 &&
              fs::exists(models / "A-00001-of-00002.gguf") && !fs::exists(models / "A-00001-of-00002.gguf.deleting") &&
              err.find("nothing was deleted") != std::string::npos,
          "delete: a set that cannot go whole is left whole, and the reason says so: " + err);
    fs::remove_all(dir, ec);
}

int main() {
    protocol_tests();
    block_stream_tests();
    anthropic_tests();
    responses_tests();
    queue_tests();
    settings_tests();
    planning_config_tests();
    json_tests();
    access_tests();
    file_tests();
    decide_tests();
    model_files_tests();
    hf_tests();
    plan_view_tests();
    download_tests();
    event_bus_tests();
    url_entry_tests();
    model_delete_tests();
    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall server policy checks passed\n");
    return 0;
}
