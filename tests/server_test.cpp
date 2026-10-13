// Unit tests for bmoe-server's policy (server/): the rules that decide what the engine is fed and
// which value wins, driven without HTTP, a session or a model.
//
//   * the chat protocol: which request fields become an engine turn, and what a turn's result
//     (text, tool calls, token counts) looks like on the wire;
//   * the turn queue: first come first served, a bound, and a caller that gives its place up;
//   * the settings layers: defaults < plan < launch < user, what a reset removes, what persists;
//   * the JSON edges: form values to parameter strings and back.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "json_util.h"
#include "openai_chat.h"
#include "settings_store.h"
#include "turn_queue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace bmoe;
using namespace bmoe::server;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

static OpenAiChatRequest parsed(const char * body, bool * ok = nullptr, std::string * err = nullptr) {
    OpenAiChatRequest r;
    std::string e;
    RunConfig cfg;
    cfg.n_predict = 100;
    cfg.think = true;
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
        check(r.turn.n_predict == 100 && r.turn.think && !r.stream && !r.include_usage && r.turn.tools.empty(),
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
    check(!parsed(R"j({"messages":[{"role":"user","content":"hi"}],"think":false})j").turn.think &&
              !parsed(R"j({"messages":[{"role":"user","content":"hi"}],"reasoning_effort":"none"})j").turn.think &&
              parsed(R"j({"messages":[{"role":"user","content":"hi"}],"reasoning_effort":"low"})j").turn.think &&
              parsed(R"j({"messages":[{"role":"user","content":"hi"}],"reasoning_effort":"none","think":true})j")
                  .turn.think,
          "thinking: the server's own field wins, and only effort none turns it off");

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
    plain.cancelled = true;
    check(finish_reason(plain, 4) == "cancelled", "a cancelled turn says so");
    check(completion_json(meta, plain, "stop")["choices"][0]["message"]["content"] == "hi" &&
              !completion_json(meta, plain, "stop")["choices"][0]["message"].contains("tool_calls"),
          "an ordinary answer has text and no calls");
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

    json rejected = s.apply(json{{"threads", 12}, {"top-k", "abc"}, {"nope", 1}}, {});
    check(s.config().n_threads == 12, "user values override launch flags");
    check(rejected.contains("top-k") && rejected.contains("nope") && !rejected.contains("threads"),
          "unparseable values and unknown keys are rejected, the rest applied");
    const auto keys = s.operator_keys();
    check(keys.size() == 1 && keys[0] == "threads", "operator keys: launch and user");

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
}

int main() {
    protocol_tests();
    queue_tests();
    settings_tests();
    json_tests();
    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall server policy checks passed\n");
    return 0;
}
