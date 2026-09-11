// Unit tests for bmoe-server's policy (server/): the rules that decide what the engine is fed and
// which value wins, driven without HTTP, a session or a model.
//
//   * conversation continuity (plan_turn): when the KV is continued, and what a fresh start drops;
//   * the settings layers: defaults < plan < launch < user, what a reset removes, what persists;
//   * the JSON edges: form values to parameter strings and back, and text as a client receives it.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "conversation.h"
#include "json_util.h"
#include "settings_store.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace bmoe;
using namespace bmoe::server;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

static ChatMessage u(const char * s) {
    return {"user", s};
}
static ChatMessage a(const char * s) {
    return {"assistant", s};
}
static ChatMessage sys(const char * s) {
    return {"system", s};
}

static void conversation_tests() {
    {
        const TurnPlan t = plan_turn({}, {u("hi")});
        check(t.ok && t.clear_kv && t.prompt == "hi" && !t.history_dropped, "first message starts fresh");
    }
    {
        const TurnPlan t = plan_turn({u("hi"), a("hello")}, {u("hi"), a("hello"), u("more")});
        check(t.ok && !t.clear_kv && t.prompt == "more", "exact extension continues the KV with the new message only");
    }
    {
        const TurnPlan t = plan_turn({u("hi"), a("hello")}, {u("hi"), a("HELLO"), u("more")});
        check(t.clear_kv && t.prompt == "more" && t.history_dropped, "an edited answer starts fresh and says so");
    }
    {
        const TurnPlan t = plan_turn({u("hi"), a("hello")}, {u("hi"), a("hello"), u("x"), a("y"), u("z")});
        check(t.clear_kv && t.history_dropped, "a history longer by more than one turn starts fresh");
    }
    {
        const TurnPlan t = plan_turn({u("hi"), a("hello")}, {u("hi")});
        check(t.clear_kv && !t.history_dropped && t.prompt == "hi",
              "a regenerated first turn starts fresh, drops nothing");
    }
    {
        const TurnPlan t = plan_turn({}, {sys("be brief"), u("hi")});
        check(t.clear_kv && t.prompt == "be brief\n\nhi" && !t.history_dropped,
              "a system message rides with the first user turn");
    }
    {
        const TurnPlan t = plan_turn({sys("s"), u("hi"), a("ok")}, {sys("s"), u("hi"), a("ok"), u("next")});
        check(!t.clear_kv && t.prompt == "next", "a conversation with a system message continues too");
    }
    {
        const TurnPlan t = plan_turn({}, {a("only an answer")});
        check(!t.ok, "no user message is an error");
    }
    {
        const TurnPlan t = plan_turn({u("hi"), a("hello")}, {u("hi"), a("hello"), a("again")});
        check(t.clear_kv && t.prompt == "hi" && t.history_dropped == false,
              "ending on an assistant turn restarts from the last user message");
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

    check(utf8_as_sent("plain") == "plain", "valid UTF-8 is unchanged");
    check(utf8_as_sent(std::string("ab\xE2\x82", 4)) == "ab\xEF\xBF\xBD", "a cut multi-byte character becomes U+FFFD");
}

int main() {
    conversation_tests();
    settings_tests();
    json_tests();
    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall server policy checks passed\n");
    return 0;
}
