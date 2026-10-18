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
//   * model files: which names make a split set, and how a directory's files group into models.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "access.h"
#include "decide_protocol.h"
#include "json_util.h"
#include "model_files.h"
#include "openai_chat.h"
#include "settings_store.h"
#include "turn_queue.h"

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
    // Every failure kind has its own stable name, and only Failed closes the session.
    {
        const RunError kinds[] = {
            RunError::None,          RunError::ContextFull,    RunError::EmptyPrompt, RunError::TemplateRejected,
            RunError::NeedsChatMode, RunError::InvalidRequest, RunError::Failed};
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
}

static void access_tests() {
    check(is_api_path("/api/info") && is_api_path("/v1/chat/completions") && !is_api_path("/") &&
              !is_api_path("/assets/app.js") && !is_api_path("/apix"),
          "the API is /api/* and /v1/*, nothing else");
    check(is_loopback_host("127.0.0.1") && is_loopback_host("localhost:8765") && is_loopback_host("[::1]:8765") &&
              is_loopback_host("::1") && !is_loopback_host("0.0.0.0") && !is_loopback_host("example.com:8765") &&
              !is_loopback_host("192.168.1.10"),
          "loopback host names, with or without a port");

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
}

int main() {
    protocol_tests();
    queue_tests();
    settings_tests();
    planning_config_tests();
    json_tests();
    access_tests();
    file_tests();
    decide_tests();
    model_files_tests();
    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall server policy checks passed\n");
    return 0;
}
