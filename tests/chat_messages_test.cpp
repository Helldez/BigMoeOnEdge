// Tool calls through the engine's chat types, with no model: bmoe/chat.h messages and tools go
// into a real vendored template, a call the model would write comes back out as a ChatToolCall, and
// the template's grammar (run over llama.cpp's vendored vocab file) holds a call to that syntax.
//
// What is pinned here is the wiring the session relies on (core/src/engine/chat_messages.h), not
// any model's syntax - that is llama.cpp's, and its own tests cover it. If a submodule bump changes
// how a template renders tools or what the parser returns for a call, this is where it shows.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "bmoe/chat.h"
#include "chat_messages.h"
#include "chat_parse.h"
#include "thinking_control.h"

#include "common.h"
#include "llama.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace bmoe;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

static std::string read_file(const char * path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool has(const std::string & hay, const char * needle) {
    return hay.find(needle) != std::string::npos;
}

// Walks `text`'s tokens through the grammar the way the decode loop does: each one checked, then
// accepted. Returns how many it took before refusing one, or -1 when it took them all.
static int first_refused(detail::ToolGrammar & g, const llama_vocab * vocab, const std::string & text) {
    const std::vector<llama_token> toks = common_tokenize(vocab, text, /*add_special*/ false, /*parse_special*/ true);
    for (size_t i = 0; i < toks.size(); ++i) {
        if (!g.allows(toks[i])) return (int) i;
        g.accept(toks[i]);
        if (!g.active()) return (int) i;
    }
    return -1;
}

static void response_format_checks(common_chat_templates * tmpls,
                                   const llama_vocab * vocab,
                                   const std::vector<ChatMessage> & convo,
                                   const std::vector<ChatTool> & tools,
                                   const std::string & call_text,
                                   bool auto_parser) {
    using json = nlohmann::ordered_json;
    std::printf("\nResponse-format checks: %s\n", auto_parser ? "differential parser" : "Qwen3.5 specialized parser");
    auto render = [&](const std::string & schema, bool think = false, ThinkControl ctl = ThinkControl::Template,
                      ToolChoice choice = ToolChoice::Auto, bool offer_tools = false) {
        common_chat_templates_inputs inputs;
        detail::build_turn_inputs(inputs, detail::to_common_messages(convo), think, ctl);
        detail::add_tools(inputs, offer_tools ? tools : std::vector<ChatTool>{}, choice);
        inputs.json_schema = schema;
        return common_chat_templates_apply(tmpls, inputs);
    };
    auto accepts = [&](const common_chat_params & cp, const std::string & text, bool complete = true) {
        detail::ToolGrammar g;
        std::string err;
        if (!g.init(cp, vocab, err) || !g.active()) {
            std::printf("  structured grammar: %s\n", err.c_str());
            return false;
        }
        return first_refused(g, vocab, text) == -1 && (!complete || g.allows(llama_vocab_eos(vocab)));
    };
    auto parses = [&](const common_chat_params & cp, const std::string & text, const std::string & expected,
                      bool reasoning = false) {
        try {
            const auto msg = common_chat_parse(common_chat_input(text), false, detail::build_parse_params(cp));
            return msg.tool_calls.empty() && !msg.content.empty() &&
                   json::parse(msg.content) == json::parse(expected) && (!reasoning || !msg.reasoning_content.empty());
        } catch (const std::exception & e) {
            std::printf("  structured parse: %s\n", e.what());
            return false;
        }
    };

    const std::string object_schema = R"({"type":"object"})";
    const auto object = render(object_schema);
    check(!object.grammar.empty() && !object.grammar_lazy, "response format uses the template's non-lazy grammar");
    check(accepts(object, R"({"answer":"Paris"})") && parses(object, R"({"answer":"Paris"})", R"({"answer":"Paris"})"),
          "JSON object passes both the template grammar and response parser");
    check(!accepts(object, "[1,2]"), "object response format refuses an array");
    check(!accepts(object, R"({"answer":})"), "object response format refuses malformed JSON");
    check(!accepts(object, R"({"answer":"Paris"} trailing)"), "object response format refuses trailing prose");

    const auto typed = render(
        R"({"type":"object","properties":{"answer":{"const":"Paris"},"count":{"type":"integer"}},"required":["answer","count"],"additionalProperties":false})");
    const std::string typed_answer = R"({"answer":"Paris","count":2})";
    check(accepts(typed, typed_answer) && parses(typed, typed_answer, typed_answer),
          "typed schema uses the same template grammar and parser");
    check(!accepts(typed, R"({"answer":"London","count":2})"), "schema grammar enforces a constant");
    check(!accepts(typed, R"({"answer":"Paris","count":"2"})"), "schema grammar enforces field types");
    check(!accepts(typed, R"({"answer":"Paris"})"), "schema grammar refuses a missing required field");
    check(!accepts(typed, R"({"answer":"Paris","count":2,"extra":true})"),
          "schema grammar refuses an additional property");

    const auto array = render(R"({"type":"array","items":{"type":"integer"},"minItems":1})");
    check(accepts(array, "[1,2]") && parses(array, "[1,2]", "[1,2]"),
          "a schema may request an array instead of an object");
    check(!accepts(array, R"({"value":1})") && !accepts(array, R"(["1"])"),
          "array schema refuses an object and a wrongly typed item");

    const std::string literal =
        R"({"text":"<think>不是思考</think> <tool_call> 🦙 café","escaped":"quote: \"; slash: \\; newline: \n"})";
    check(accepts(object, literal) && parses(object, literal, literal),
          "JSON string data keeps reasoning/tool markers, UTF-8, and escapes");
    if (auto_parser) {
        check(accepts(object, "```json\n" + typed_answer + "\n```") &&
                  parses(object, "```json\n" + typed_answer + "\n```", typed_answer),
              "the differential parser's fenced JSON form returns only its JSON content");
    } else {
        check(!accepts(object, "```json\n" + typed_answer + "\n```"),
              "the specialized parser constrains JSON directly without a Markdown fence");
    }

    const auto thinking = render(object_schema, true);
    const std::string open = thinking.generation_prompt.find(thinking.thinking_start_tag) == std::string::npos
                                 ? thinking.thinking_start_tag + "\n"
                                 : "";
    const std::string close = thinking.thinking_end_tags.empty() ? "" : thinking.thinking_end_tags[0];
    const std::string thought = open + "I will return a JSON object." + close + "\n" + typed_answer;
    check(!close.empty() && accepts(thinking, thought) && parses(thinking, thought, typed_answer, true),
          "reasoning precedes JSON without entering the returned content");
    {
        detail::ToolGrammar g;
        std::string err;
        check(g.init(thinking, vocab, err) && g.active() &&
                  first_refused(g, vocab, open + "I am still thinking about the answer.") == -1 &&
                  !g.allows(llama_vocab_eos(vocab)),
              "reasoning alone cannot complete a structured response");
    }
    const auto prefilled = render(object_schema, false, ThinkControl::Prefill);
    check(accepts(prefilled, typed_answer) && parses(prefilled, typed_answer, typed_answer),
          "a no-think content continuation starts the grammar at the JSON answer");

    for (const auto choice : {ToolChoice::Auto, ToolChoice::Required, ToolChoice::None}) {
        const auto with_tools = render(object_schema, false, ThinkControl::Template, choice, true);
        const std::string name = choice == ToolChoice::Auto       ? "auto"
                                 : choice == ToolChoice::Required ? "required"
                                                                  : "none";
        if (!auto_parser && choice == ToolChoice::Auto) {
            // This pinned specialized parser marks the response grammar lazy even though it has
            // no tool trigger. Keep the concrete failure visible: the session must not fall back
            // to unconstrained generation when a structured response was requested.
            detail::ToolGrammar g;
            std::string err;
            check(with_tools.grammar_lazy && !g.init(with_tools, vocab, err) && !g.active() && !err.empty(),
                  "Qwen3.5 JSON plus tools:auto reports the upstream invalid lazy grammar");
            continue;
        }
        check(accepts(with_tools, typed_answer) && parses(with_tools, typed_answer, typed_answer),
              "JSON remains constrained with offered tools and tool_choice=" + name);
        check(!call_text.empty() && !accepts(with_tools, call_text),
              "upstream response format takes precedence over a tool call (tool_choice=" + name + ")");
    }

    {
        detail::ToolGrammar g;
        std::string err;
        check(g.init(object, vocab, err) && g.active() && first_refused(g, vocab, R"({"answer":)") == -1 &&
                  !g.allows(llama_vocab_eos(vocab)),
              "an unfinished object cannot end at EOS");
    }
    {
        detail::ToolGrammar g;
        std::string err;
        check(g.init(object, vocab, err) && g.active() && first_refused(g, vocab, "{}") == -1 &&
                  g.allows(llama_vocab_eos(vocab)),
              "a complete object allows EOS");
    }
    // Each request owns a fresh grammar; a completed or abandoned request must not affect the next.
    check(accepts(typed, typed_answer) && accepts(array, "[3]") && accepts(object, "{}"),
          "consecutive independent schemas do not share grammar state");
    {
        detail::ToolGrammar g;
        std::string err;
        const auto unconstrained = render("");
        const auto tokens = common_tokenize(vocab, "Plain text again.", false, true);
        check(unconstrained.grammar.empty() && g.init(unconstrained, vocab, err) && !g.active() && !tokens.empty() &&
                  g.allows(tokens.front()),
              "the next unconstrained request has no leftover JSON grammar");
    }
    {
        detail::ToolGrammar g;
        std::string err;
        common_chat_params invalid = object;
        invalid.grammar = "root ::= missing-rule\n";
        check(!g.init(invalid, vocab, err) && !g.active() && !err.empty(),
              "an invalid template grammar is reported at initialization");
    }
}

static void completed_json_checks() {
    common_chat_msg msg;
    std::string err;
    check(!detail::validate_json_response(msg, err) && !err.empty(),
          "a completed structured turn with no content is an error");
    msg.reasoning_content = "Reasoning does not replace the requested JSON.";
    check(!detail::validate_json_response(msg, err), "a reasoning-only completion is not a JSON answer");
    msg.content = "[1,true,null]";
    check(detail::validate_json_response(msg, err) && err.empty(),
          "completed JSON validation accepts schema-selected arrays and clears a previous error");
    msg.content = R"({"broken":)";
    check(!detail::validate_json_response(msg, err) && !err.empty(),
          "a malformed completed response is explicitly rejected");
    msg.content = "{} trailing prose";
    check(!detail::validate_json_response(msg, err), "completed JSON validation rejects trailing prose");
    msg.content = std::string("{\"text\":\"") + char(0xff) + "\"}";
    check(!detail::validate_json_response(msg, err), "completed JSON validation rejects invalid UTF-8");
    msg.content.clear();
    common_chat_tool_call call;
    call.name = "read_file";
    call.arguments = R"({"path":"calc.py"})";
    msg.tool_calls.push_back(call);
    check(detail::validate_json_response(msg, err) && err.empty(),
          "a tool-call-only turn does not require a JSON content field");
    msg.content = "not JSON";
    check(!detail::validate_json_response(msg, err), "a tool call cannot excuse malformed answer content");
}

int main() {
    completed_json_checks();
    const std::string tmpl = read_file(BMOE_QWEN3_TEMPLATE_PATH);
    check(!tmpl.empty(), "the vendored template is readable");
    auto tmpls = common_chat_templates_init(/*model=*/nullptr, tmpl);

    const std::vector<ChatTool> tools = {
        {"read_file", "Read a file from disk",
         R"j({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})j"}};

    // ── a first turn with tools on offer ──
    std::vector<ChatMessage> convo = {{"system", "You fix code.", "", {}, "", ""},
                                      {"user", "What is in calc.py?", "", {}, "", ""}};
    common_chat_templates_inputs inputs;
    detail::build_turn_inputs(inputs, detail::to_common_messages(convo), /*think*/ false, ThinkControl::Template);
    detail::add_tools(inputs, tools, ToolChoice::Auto);
    common_chat_params cp = common_chat_templates_apply(tmpls.get(), inputs);
    const std::string p1 = cp.prompt.str();
    check(has(p1, "read_file") && has(p1, "Read a file from disk") && has(p1, "You fix code."),
          "the template describes the tool and keeps the system message");

    // No tools on offer: the same conversation renders without them.
    common_chat_templates_inputs bare;
    detail::build_turn_inputs(bare, detail::to_common_messages(convo), false, ThinkControl::Template);
    detail::add_tools(bare, {}, ToolChoice::Auto);
    check(!has(common_chat_templates_apply(tmpls.get(), bare).prompt.str(), "read_file"),
          "a request with no tools mentions none");

    // ── the model's call, read back ──
    const common_chat_parser_params pp = detail::build_parse_params(cp);
    const std::string raw =
        "<tool_call>\n<function=read_file>\n<parameter=path>\ncalc.py\n</parameter>\n</function>\n</tool_call>";
    const std::string raw_json =
        "<tool_call>\n{\"name\": \"read_file\", \"arguments\": {\"path\": \"calc.py\"}}\n</tool_call>";
    std::vector<ChatToolCall> calls;
    std::string content = "?";
    std::string call_text; // the candidate this template's parser reads
    // The call syntax is the template's own; which of the two this template uses is not this
    // test's to know, so it takes the one the parser accepts.
    for (const std::string & candidate : {raw, raw_json}) {
        try {
            const common_chat_msg msg = common_chat_parse(common_chat_input(candidate), /*is_partial*/ false, pp);
            if (msg.tool_calls.empty()) continue;
            calls = detail::from_common_tool_calls(msg.tool_calls);
            content = msg.content;
            call_text = candidate;
            // Mid-call, the streaming view must not show the call's text as the answer.
            const common_chat_msg part =
                common_chat_parse(common_chat_input(candidate.substr(0, candidate.size() / 2)), true, pp);
            check(!has(part.content, "read_file") && !has(part.content, "tool_call"),
                  "a call in progress does not leak into the answer");
            break;
        } catch (const std::exception &) {
        }
    }
    check(calls.size() == 1 && calls[0].name == "read_file", "the call is parsed out of the generation");
    check(calls.size() == 1 && has(calls[0].arguments, "calc.py") && calls[0].arguments.front() == '{',
          "its arguments are a JSON object naming the file");
    check(content.empty(), "a turn that only calls has no answer text");

    // ── the grammar the template ships with its tools (#334) ──
    llama_model_params mp = llama_model_default_params();
    mp.vocab_only = true;
    llama_model * vocab_model = llama_model_load_from_file(BMOE_QWEN35_VOCAB_PATH, mp);
    check(vocab_model != nullptr, "the vendored vocab file loads");
    if (vocab_model) {
        const llama_vocab * vocab = llama_model_get_vocab(vocab_model);
        check(!cp.grammar.empty() && cp.grammar_lazy, "a turn with tools carries a lazy grammar");
        auto fresh = [&](detail::ToolGrammar & g) {
            std::string err;
            const bool ok = g.init(cp, vocab, err);
            if (!ok) std::printf("  grammar: %s\n", err.c_str());
            return ok && g.active();
        };
        {
            detail::ToolGrammar g;
            check(fresh(g), "the grammar builds");
            check(first_refused(g, vocab, "Sure, let me look at calc.py first.") == -1, "free text is not constrained");
        }
        {
            detail::ToolGrammar g;
            fresh(g);
            check(!call_text.empty() && first_refused(g, vocab, call_text) == -1,
                  "a well-formed call passes the grammar token by token");
        }
        {
            // A call to a tool the request never offered is what the grammar exists to stop.
            std::string bad = call_text;
            const size_t at = bad.find("read_file");
            if (at != std::string::npos) bad.replace(at, 9, "drop_table");
            detail::ToolGrammar g;
            fresh(g);
            check(at != std::string::npos && first_refused(g, vocab, bad) > 0,
                  "a call to a tool that was not offered is refused once the call has opened");
        }
        {
            // With thinking on, a call's opening written while the model reasons is not a call: the
            // span stays free, and the grammar takes over once it closes.
            common_chat_templates_inputs think_in;
            detail::build_turn_inputs(think_in, detail::to_common_messages(convo), /*think*/ true,
                                      ThinkControl::Template);
            detail::add_tools(think_in, tools, ToolChoice::Auto);
            const common_chat_params tcp = common_chat_templates_apply(tmpls.get(), think_in);
            std::string bad = call_text;
            const size_t at = bad.find("read_file");
            if (at != std::string::npos) bad.replace(at, 9, "drop_table");
            const std::string reasoning = "I could write " + bad + " but I should not.\n";
            const std::string close = tcp.thinking_end_tags.empty() ? "" : tcp.thinking_end_tags[0];
            std::string err;
            detail::ToolGrammar g;
            const bool ok = g.init(tcp, vocab, err) && g.active() && !close.empty();
            check(ok && first_refused(g, vocab, reasoning + close + "\n\n") == -1,
                  "a call's syntax inside the reasoning span is not constrained");
            check(ok && first_refused(g, vocab, bad) > 0, "after the span closes, a bad call is refused");
        }
        {
            common_chat_params none = cp;
            none.grammar.clear();
            detail::ToolGrammar g;
            std::string err;
            check(g.init(none, vocab, err) && !g.active(), "a turn without a grammar runs unconstrained");
        }
        response_format_checks(tmpls.get(), vocab, convo, tools, call_text, false);
        const std::string auto_tmpl = read_file(BMOE_QWEN3_AUTO_TEMPLATE_PATH);
        check(!auto_tmpl.empty(), "the vendored differential-parser template is readable");
        if (!auto_tmpl.empty()) {
            auto auto_tmpls = common_chat_templates_init(nullptr, auto_tmpl);
            response_format_checks(auto_tmpls.get(), vocab, convo, tools, raw_json, true);
        }
        llama_model_free(vocab_model);
    }

    // ── the parser's empty result falls back to the raw text (#334) ──
    common_chat_msg nothing;
    check(detail::parsed_nothing(nothing), "a message with no answer, reasoning or call is read as nothing");
    common_chat_msg some;
    some.reasoning_content = "thinking";
    check(!detail::parsed_nothing(some), "reasoning alone is something");

    // ── the next turn: the call and the tool's answer go back in ──
    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.tool_calls = {{"call_1", "read_file", calls.empty() ? "{}" : calls[0].arguments}};
    ChatMessage result;
    result.role = "tool";
    result.tool_call_id = "call_1";
    result.name = "read_file";
    result.content = "def median(values): ...";
    convo.push_back(assistant);
    convo.push_back(result);
    common_chat_templates_inputs next;
    detail::build_turn_inputs(next, detail::to_common_messages(convo), false, ThinkControl::Template);
    detail::add_tools(next, tools, ToolChoice::Auto);
    const std::string p2 = common_chat_templates_apply(tmpls.get(), next).prompt.str();
    check(has(p2, "def median(values): ...") && has(p2, "calc.py"),
          "the earlier call and the tool's answer are rendered into the next prompt");
    // What makes the KV reusable across an agent's steps: the next prompt begins with everything
    // the previous one held, up to where its answer started.
    check(p2.compare(0, p1.size() - cp.generation_prompt.size(), p1, 0, p1.size() - cp.generation_prompt.size()) == 0,
          "the next prompt extends the previous one, so its prefix is reusable");

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall chat message checks passed\n");
    return 0;
}
