#include "agent.h"
#include "agent_run.h"

#include "agent_tools.h"
#include "provider_protocol.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace bmoe::server::agent {

namespace {

// The categories the mode rules apply to: a read runs, a change runs, waits for a person or is
// refused, a skill is the agent's own instructions. An MCP server's tool is a change: what it does
// is the server's, unknown here.
enum class Kind { Read, Change, Skill };

struct ToolSpec {
    const char * name;
    Kind kind;
    const char * description;
    const char * parameters;
};

// The tools as the model reads them: llama.cpp's server tools, by name, parameters and meaning, so
// a model used to one works with the other. Paths are the workspace's.
const ToolSpec kTools[] = {
    {"read_file", Kind::Read,
     "Read the contents of a file. Optionally specify a 1-based line range. If append_loc is true, each line is "
     "prefixed with its line number (e.g. \"1\xE2\x86\x92...\").",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"Path to the file"},"start_line":{"type":"integer","description":"First line to read, 1-based (default: 1)"},"end_line":{"type":"integer","description":"Last line to read, 1-based inclusive (default: end of file)"},"append_loc":{"type":"boolean","description":"Prefix each line with its line number"}},"required":["path"]})j"},
    {"file_glob_search", Kind::Read,
     "Recursively search for files matching a glob pattern under a directory. Skips common junk directories (.git, "
     "node_modules, build, dist, etc.). A pattern with no '/' (e.g. \"*.cpp\") matches the file's basename at any "
     "depth. A pattern containing '/' matches the full relative path; unless anchored with \"**/\" or a leading "
     "'/', it is prefixed with \"**/\". Use type=\"dir\" or \"all\" to also list directories; directory entries end "
     "with '/'.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"Base directory to search in"},"include":{"type":"string","description":"Glob pattern for files to include (e.g. \"*.cpp\" or \"src/**/*.cpp\"). Default: **"},"exclude":{"type":"string","description":"Glob pattern for files to exclude"},"type":{"type":"string","description":"Entry type to return: \"file\" (default), \"dir\" or \"all\""},"max_depth":{"type":"integer","description":"Maximum depth to descend into subdirectories (default: 0 = unlimited; 1 = direct children only)"},"limit":{"type":"integer","description":"Maximum number of results to return, capped at 100 (default 100)"}},"required":["path"]})j"},
    {"grep_search", Kind::Read,
     "Search for a pattern in files under a path. Returns matching lines with file paths (relative to the given "
     "directory). Skips common junk directories. include/exclude: a pattern with no '/' matches the basename at any "
     "depth; a pattern containing '/' matches the full relative path.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"File or directory to search in"},"pattern":{"type":"string","description":"Pattern to search for (regular expression unless literal is true)"},"include":{"type":"string","description":"Glob pattern to filter files (default: **)"},"exclude":{"type":"string","description":"Glob pattern to exclude files"},"return_line_numbers":{"type":"boolean","description":"If true, include line numbers in results"},"literal":{"type":"boolean","description":"Treat pattern as a literal string instead of a regular expression (default: false)"},"ignore_case":{"type":"boolean","description":"Case-insensitive search (default: false)"},"context_lines":{"type":"integer","description":"Number of lines of context to show before and after each match (default: 0)"}},"required":["path","pattern"]})j"},
    {"get_info", Kind::Read, "Returns runtime info: the OS name/version and the current working directory",
     R"j({"type":"object","properties":{}})j"},
    {"write_file", Kind::Change,
     "Write content to a file, creating it (including parent directories) if it does not exist. May use with "
     "edit_file for more complex edits.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"Path of the file to write"},"content":{"type":"string","description":"Content to write"}},"required":["path","content"]})j"},
    {"edit_file", Kind::Change,
     "Edit a file using exact text replacement. Each edits[].old_text must be unique in the file and is matched "
     "against the original content, not incrementally. Merge nearby changes into one edit instead of overlapping "
     "edits. Use write_file to replace the whole file.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to edit"},"edits":{"type":"array","description":"One or more exact text replacements to apply","items":{"type":"object","properties":{"old_text":{"type":"string","description":"Exact text to find; must be unique in the file and must not overlap with other edits"},"new_text":{"type":"string","description":"Text to replace old_text with"}},"required":["old_text","new_text"]}}},"required":["path","edits"]})j"},
    {"exec_shell_command", Kind::Change, "Execute a shell command and return its output (stdout and stderr combined).",
     R"j({"type":"object","properties":{"command":{"type":"string","description":"Shell command to execute"},"timeout":{"type":"integer","description":"Timeout in seconds (default 10, max 120)"},"max_output_size":{"type":"integer","description":"Maximum output size in bytes (default 12288)"}},"required":["command"]})j"},
    {"use_skill", Kind::Skill, "Read the instructions of one of the skills listed in the system prompt.",
     R"j({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})j"},
};

const ToolSpec * find_tool(const std::string & name) {
    for (const ToolSpec & t : kTools)
        if (name == t.name) return &t;
    return nullptr;
}

bool starts_with(const std::string & s, const std::string & p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::string arg(const json & args, const char * key) {
    return args.contains(key) && args[key].is_string() ? args[key].get<std::string>() : std::string();
}

std::string short_text(const std::string & s, size_t max_chars) {
    std::string line = s.substr(0, s.find('\n'));
    if (line.size() <= max_chars) return line;
    line.resize(complete_utf8_prefix_size(line.substr(0, max_chars)));
    return line + "...";
}

std::string title_for(const std::string & tool, const json & a) {
    if (tool == "read_file") return "Read " + arg(a, "path");
    if (tool == "file_glob_search") {
        const std::string inc = arg(a, "include");
        const std::string where = arg(a, "path").empty() ? std::string(".") : arg(a, "path");
        return inc.empty() || inc == "**" ? "List " + where : "Find " + short_text(inc, 40) + " in " + where;
    }
    if (tool == "grep_search") return "Search for \"" + short_text(arg(a, "pattern"), 40) + "\"";
    if (tool == "get_info") return "Get the system's info";
    if (tool == "write_file") return "Write " + arg(a, "path");
    if (tool == "edit_file") return "Edit " + arg(a, "path");
    if (tool == "exec_shell_command") return "Run " + short_text(arg(a, "command"), 60);
    if (tool == "use_skill") return "Use the skill " + arg(a, "name");
    return tool;
}

ToolOutput fail(const std::string & why) {
    return {false, "Error: " + why, "", ""};
}

// The change write_file or edit_file would make: the new content and its diff. Run before an
// approval, so the card shows exactly what a yes will write, and again after it.
bool plan_change(const std::string & ws,
                 const std::string & tool,
                 const json & a,
                 std::string & path,
                 std::string & content,
                 std::string & diff,
                 std::string & err) {
    if (!resolve_in_workspace(ws, arg(a, "path"), path, err)) return false;
    std::error_code ec;
    if (fs::is_directory(fs::u8path(path), ec)) {
        err = arg(a, "path") + " is a folder";
        return false;
    }
    std::string before;
    const bool exists = fs::exists(fs::u8path(path), ec);
    if (exists && !read_whole(path, before, err)) return false;
    if (tool == "write_file") {
        if (!a.contains("content") || !a["content"].is_string()) {
            err = "content is missing";
            return false;
        }
        content = a["content"].get<std::string>();
    } else {
        if (!exists) {
            err = arg(a, "path") + " does not exist: create it with write_file";
            return false;
        }
        if (!apply_edits(before, a.value("edits", json()), content, err)) {
            err += " (in " + arg(a, "path") + ")";
            return false;
        }
    }
    diff = unified_diff(workspace_relative(ws, path), before, content);
    return true;
}

// The AGENTS.md at the workspace's root, the instructions a project keeps for any coding agent:
// cut at k_agents_md_max_bytes, "" when there is none.
std::string agents_md(const std::string & ws) {
    std::string path, err, text;
    if (ws.empty() || !resolve_in_workspace(ws, "AGENTS.md", path, err)) return "";
    std::error_code ec;
    if (!fs::is_regular_file(fs::u8path(path), ec) || !read_whole(path, text, err) || looks_binary(text)) return "";
    return cap_text(text, k_agents_md_max_bytes);
}

std::string today() {
    const std::tm tm = platform::local_time(std::time(nullptr));
    char buf[64];
    std::strftime(buf, sizeof(buf), "%A %Y-%m-%d", &tm);
    return buf;
}

const char * mode_words(const std::string & mode) {
    if (mode == "full") return "Mode: full. Changes and commands run without asking.";
    if (mode == "read_only")
        return "Mode: read-only. Changes are refused and only commands that read run; say what you would change.";
    return "Mode: ask. Reading needs no permission; a change, a command that could change something and a connected "
           "tool wait for the person's approval. If one is declined, do not retry it: say what you would have done.";
}

} // namespace

Agent::Agent(AgentDeps deps) : d_(std::move(deps)) {
    if (!d_.run_command) d_.run_command = platform::run_command;
}

Agent::~Agent() {
    shutdown();
}

void Agent::start_scheduler() {
    std::lock_guard<std::mutex> lk(m_);
    if (scheduler_.joinable() || shutting_down_) return;
    scheduler_ = std::thread([this] { scheduler_loop(); });
}

void Agent::shutdown() {
    std::vector<std::shared_ptr<Run>> runs;
    {
        std::lock_guard<std::mutex> lk(m_);
        shutting_down_ = true;
        for (auto & [id, r] : runs_)
            runs.push_back(r);
    }
    {
        std::lock_guard<std::mutex> lk(sched_m_);
        sched_wake_ = true;
    }
    sched_cv_.notify_all();
    if (scheduler_.joinable()) scheduler_.join();
    for (const auto & r : runs) {
        r->stop = true;
        std::lock_guard<std::mutex> lk(r->m);
        r->cv.notify_all();
    }
    // Every thread a run ever started, finished or not: joined here, so nothing of the agent outlives
    // this call.
    std::vector<std::thread> threads;
    {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return active_ == 0; });
        for (auto & [id, r] : finished_)
            if (r->thread.joinable()) threads.push_back(std::move(r->thread));
        finished_.clear();
    }
    for (std::thread & t : threads)
        t.join();
    // The other agents' programs, now that no run talks to them.
    std::vector<std::string> open;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (const auto & [id, e] : externals_)
            open.push_back(id);
    }
    for (const std::string & id : open)
        close_external(id);
    if (d_.mcp) d_.mcp->shutdown();
}

// ---- state and events ----

std::string Agent::live_state(const std::string & id, const std::string & stored) const {
    std::shared_ptr<Run> r;
    {
        std::lock_guard<std::mutex> lk(m_);
        const auto it = runs_.find(id);
        if (it == runs_.end()) return stored == "error" ? "error" : "idle";
        r = it->second;
    }
    std::lock_guard<std::mutex> lk(r->m);
    return r->waiting_in.empty() ? "running" : "waiting";
}

bool Agent::conversation(const std::string & id, json & out) const {
    if (!d_.store->conversation(id, out)) return false;
    out["state"] = live_state(id, out.value("state", "idle"));
    return true;
}

json Agent::list_conversations() const {
    json list = d_.store->list_conversations();
    for (json & c : list)
        c["state"] = live_state(c.value("id", ""), c.value("state", "idle"));
    return list;
}

bool Agent::running(const std::string & conversation) const {
    std::lock_guard<std::mutex> lk(m_);
    return runs_.count(conversation) > 0;
}

EventBus::Audience conversation_audience(const json & conv) {
    return conversation_reaches(conv) ? EventBus::Audience::LocalOnly : EventBus::Audience::Everyone;
}

EventBus::Audience Agent::audience_of(const std::string & conv) const {
    return d_.store->reaches(conv) ? EventBus::Audience::LocalOnly : EventBus::Audience::Everyone;
}

void Agent::publish_conversation(const std::string & id) {
    json c;
    if (!conversation(id, c)) return;
    d_.bus->publish("conversation",
                    json{{"id", id},
                         {"title", c.value("title", "")},
                         {"state", c.value("state", "idle")},
                         {"updated", c.value("updated", 0LL)}},
                    conversation_audience(c));
}

void Agent::publish_item(const std::string & conv, const json & item, bool persist) {
    d_.store->upsert_item(conv, item, persist);
    d_.bus->publish("item", json{{"conversation", conv}, {"item", item}}, audience_of(conv));
}

void Agent::notice(const std::string & conv, const std::string & level, const std::string & text) {
    publish_item(conv, json{{"id", new_id("i")}, {"kind", "notice"}, {"level", level}, {"text", text}});
}

std::string Agent::workspace_path(const json & conv) const {
    const json w = conv.value("workspace", json(nullptr));
    if (!w.is_string()) return "";
    json ws;
    if (!d_.store->workspace(w.get<std::string>(), ws)) return "";
    return ws.value("path", "");
}

// ---- skills ----

std::vector<std::pair<Skill, bool>> Agent::skills(const std::string & workspace_path,
                                                  std::vector<std::string> * warnings) const {
    std::vector<std::string> w;
    std::map<std::string, Skill> by_name;
    for (const auto & [dir, source] : std::vector<std::pair<std::string, std::string>>{
             {d_.bundled_skills_dir, "bundled"},
             {d_.user_skills_dir, "user"},
             {workspace_path.empty() ? std::string() : (fs::u8path(workspace_path) / ".bmoe" / "skills").u8string(),
              "workspace"}})
        for (Skill & s : scan_skills(dir, source, w))
            by_name[s.name] = std::move(s);
    if (warnings) *warnings = w;
    const std::map<std::string, bool> on = d_.store->skill_switches();
    std::vector<std::pair<Skill, bool>> out;
    for (auto & [name, s] : by_name) {
        const auto it = on.find(name);
        out.push_back({std::move(s), it == on.end() || it->second});
    }
    return out;
}

// ---- requests to the model ----

std::string Agent::system_prompt(const json & conv, bool remote) const {
    if (!d_.tools) return ""; // plain chat: the model as it is, as before the agent
    const std::string ws = remote ? std::string() : workspace_path(conv);
    // Short on purpose: every token here is prefilled on every new conversation, and a small model
    // follows a few plain rules better than many. What each tool does is in its own definition.
    std::string s = "You are a coding agent working for the person at this computer. Today is " + today() +
                    "; the "
                    "computer runs " +
                    d_.os_name + ".\n";
    if (!ws.empty()) {
        // Neither the folder's absolute path nor its name: a small model handed either prefixes it to
        // what it writes (a folder tree, or a folder of that name, inside the workspace). Commands start
        // there already; get_info gives the path when one is needed.
        s += "You work in the person's project folder, which is the working directory. Give tools paths relative "
             "to it, like `src/main.py` or `.`; nothing outside it can be reached.\n";
    } else {
        s += "No workspace: there are no file or command tools. If the person asks to create, read or change "
             "files, tell them to pick a folder for this conversation first.\n";
    }
    s += mode_words(conv.value("mode", "ask"));
    s += "\n";
    bool any = false;
    for (const auto & [skill, enabled] : skills(ws)) {
        if (!enabled) continue;
        if (!any) s += "Skills (read one with use_skill when the task matches):\n";
        any = true;
        s += "- " + skill.name + ": " + skill.description + "\n";
    }
    s += "Work step by step with the tools, check what you change, and end with a short answer that says what you "
         "did and what is left.";
    const std::string project = agents_md(ws);
    if (!project.empty()) s += "\n\nThe workspace's AGENTS.md:\n" + project;
    return s;
}

std::vector<ChatTool> Agent::tools_for(const json & conv, bool remote, std::map<std::string, McpTool> * mcp) const {
    if (!d_.tools) return {};
    const std::string ws = workspace_path(conv);
    const std::string mode = conv.value("mode", "ask");
    const bool local_ws = !ws.empty() && !remote;
    bool any_skill = false;
    for (const auto & [s, on] : skills(ws))
        any_skill = any_skill || on;
    std::vector<ChatTool> out;
    for (const ToolSpec & t : kTools) {
        const std::string n = t.name;
        bool offered = false;
        switch (t.kind) {
        case Kind::Read:
            offered = local_ws;
            break;
        case Kind::Change:
            // A change in read-only mode could only be refused, so it is not offered; a command still
            // is, since one that only reads runs.
            offered = local_ws && (n == "exec_shell_command" ? d_.can_run_commands : mode != "read_only");
            break;
        case Kind::Skill:
            offered = any_skill;
            break;
        }
        if (offered) out.push_back({t.name, t.description, t.parameters});
    }
    // An MCP server's tools, beside the built-in ones under the name `<server>_<tool>`: a name already
    // taken is left out, so a server cannot stand in for a built-in tool. Like a change, none is
    // offered where it could only be refused.
    if (d_.mcp && local_ws && mode != "read_only") {
        for (const McpTool & t : d_.mcp->tools()) {
            const std::string name = t.exposed_name();
            bool taken = false;
            for (const ChatTool & c : out)
                taken = taken || c.name == name;
            if (taken) continue;
            out.push_back({name, t.description, t.input_schema.dump()});
            if (mcp) (*mcp)[name] = t;
        }
    }
    return out;
}

ChatRequest Agent::build_request(const json & conv, const std::string & system, bool remote, bool think) const {
    ChatRequest req;
    if (!system.empty()) req.messages.push_back({"system", system});
    const json & items = conv["items"];
    // What each call got back, by its id.
    std::map<std::string, std::string> answers;
    for (const json & it : items) {
        const std::string call = it.value("call_id", "");
        if (call.empty()) continue;
        std::string r = it.value("result", "");
        if (it.value("shortened", false)) r = cap_text(r, k_shortened_result_bytes);
        answers[call] = r;
    }
    for (const json & it : items) {
        const std::string kind = it.value("kind", "");
        if (kind == "user") {
            req.messages.push_back({"user", it.value("text", "")});
        } else if (kind == "assistant") {
            ChatMessage m{"assistant", it.value("text", "")};
            if (it.contains("tool_calls") && it["tool_calls"].is_array())
                for (const json & c : it["tool_calls"])
                    m.tool_calls.push_back({c.value("id", ""), c.value("name", ""), c.value("arguments", "{}")});
            if (m.content.empty() && m.tool_calls.empty()) continue;
            req.messages.push_back(m);
            // Every call is answered, in the order it was made; one the run never reached (a stop)
            // says so, since a template may refuse a call with no answer.
            for (const ChatToolCall & c : m.tool_calls) {
                ChatMessage t{"tool", ""};
                const auto a = answers.find(c.id);
                t.content = a == answers.end() ? "Not run: the run was stopped before this call." : a->second;
                t.tool_call_id = c.id;
                t.name = c.name;
                req.messages.push_back(t);
            }
        }
    }
    req.tools = tools_for(conv, remote);
    const RunConfig cfg = d_.config ? d_.config() : RunConfig{};
    req.n_predict = cfg.n_predict;
    req.think = think;
    return req;
}

ChatOutcome Agent::ask_provider(Run & run,
                                const std::string & route,
                                const ChatRequest & req,
                                const std::function<bool(const TokenMetrics &)> & on_token) {
    ChatOutcome out;
    const size_t slash = route.find('/');
    const std::string pid = route.substr(0, slash), model = slash == std::string::npos ? "" : route.substr(slash + 1);
    json p;
    std::string key;
    if (!d_.store->provider(pid, p, key) || model.empty()) {
        out.status = 400;
        out.code = "no_provider";
        out.error = "the conversation's route " + route + " names no saved provider and model";
        return out;
    }
    if (!d_.net) {
        out.status = 503;
        out.code = "no_network";
        out.error = "this server cannot make web requests";
        return out;
    }
    std::string base = p.value("base_url", "");
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    HttpFetch::Request r;
    r.method = "POST";
    r.url = base + "/chat/completions";
    r.token = key;
    r.body = provider_request_json(req, model, true).dump(-1, ' ', false, json::error_handler_t::replace);
    r.content_type = "application/json";
    r.accept = "text/event-stream";
    r.timeout_s = k_provider_timeout_s;
    r.allow_http = true;
    ProviderStream ps;
    std::string raw; // the first bytes, for an error body that is not a stream
    int status = 0;
    std::string err;
    bool wanted = true;
    const bool ok = d_.net->stream(
        r,
        [&](const char * d, size_t n) {
            if (raw.size() < k_tool_result_max_bytes) raw.append(d, std::min(n, k_tool_result_max_bytes - raw.size()));
            ps.feed(d, n);
            if (ps.changed()) {
                TokenMetrics tm;
                tm.text = ps.text();
                tm.reasoning = ps.reasoning();
                if (!on_token(tm)) wanted = false;
            }
            return wanted && !run.stop.load();
        },
        status, err);
    if (!ok) {
        out.status = 502;
        out.code = "provider_unreachable";
        out.error = "could not reach " + p.value("name", pid) + ": " + err;
        return out;
    }
    ps.finish_body(raw);
    if (status >= 400 || (!ps.error().empty() && ps.text().empty() && ps.tool_calls().empty())) {
        out.status = 502;
        out.code = "provider_error";
        out.error = p.value("name", pid) + " answered" + (status ? " HTTP " + std::to_string(status) : "") + ": " +
                    (ps.error().empty() ? std::string("no answer") : ps.error());
        return out;
    }
    out.result.ok = true;
    out.result.cancelled = run.stop.load() || !wanted;
    out.result.generated_text = ps.text();
    out.result.reasoning_text = ps.reasoning();
    out.result.tool_calls = ps.tool_calls();
    out.n_predict = req.n_predict;
    return out;
}

ChatOutcome Agent::ask_model(Run & run,
                             const json & conv,
                             const ChatRequest & req,
                             const std::function<bool(const TokenMetrics &)> & on_token) {
    const std::string route = conv.value("route", "local");
    if (!route.empty() && route != "local") return ask_provider(run, route, req, on_token);
    // Waiting in the engine's line, a stop gives the place up.
    return d_.local_chat(req, on_token, [&run] { return !run.stop.load(); });
}

// ---- approvals ----

bool Agent::wait_approval(Run & run, const std::string & conv_id, json & item) {
    const std::string id = item.value("id", "");
    if (run.auto_deny) {
        ++run.denied;
        return false;
    }
    // The decision slot first, so an approval that follows the card at once is taken; the item next;
    // the waiting state last, so a client that reads `waiting` finds the card in the conversation.
    {
        std::lock_guard<std::mutex> lk(run.m);
        run.decisions[id] = -1;
    }
    item["state"] = "waiting";
    publish_item(conv_id, item);
    {
        std::lock_guard<std::mutex> lk(run.m);
        run.waiting_in[id] = conv_id;
    }
    publish_conversation(conv_id);
    int decision = 0;
    {
        std::unique_lock<std::mutex> lk(run.m);
        run.cv.wait(lk, [&] { return run.decisions[id] != -1 || run.stop.load(); });
        decision = run.decisions[id] == 1 && !run.stop.load() ? 1 : 0;
        run.decisions.erase(id);
        run.waiting_in.erase(id);
    }
    publish_conversation(conv_id);
    return decision == 1;
}

Refusal Agent::approve(const std::string & conversation, const std::string & item, bool approved) {
    std::shared_ptr<Run> r;
    {
        std::lock_guard<std::mutex> lk(m_);
        const auto it = runs_.find(conversation);
        if (it != runs_.end()) r = it->second;
    }
    if (!r) return {404, "nothing is waiting for an approval in this conversation", "not_found"};
    {
        std::lock_guard<std::mutex> lk(r->m);
        const auto it = r->decisions.find(item);
        if (it == r->decisions.end() || it->second != -1)
            return {404, "that item is not waiting for an approval", "not_found"};
        it->second = approved ? 1 : 0;
    }
    r->cv.notify_all();
    return {};
}

// ---- tools ----

json Agent::run_tool(Run & run, const json & conv, const ChatToolCall & call) {
    const std::string conv_id = conv.value("id", "");
    json args = json::object();
    bool args_ok = true;
    try {
        args = call.arguments.empty() ? json::object() : json::parse(call.arguments);
        if (!args.is_object()) args_ok = false;
    } catch (const std::exception &) {
        args_ok = false;
    }
    if (!args_ok) args = json::object();
    json item{{"id", new_id("i")}, {"kind", "tool"},     {"tool", call.name}, {"title", title_for(call.name, args)},
              {"args", args},      {"state", "running"}, {"result", ""},      {"call_id", call.id}};
    const auto finish = [&](const std::string & state, const std::string & result) {
        item["state"] = state;
        item["result"] = cap_text(result, k_tool_result_max_bytes);
        publish_item(conv_id, item);
        return item;
    };
    if (!args_ok) return finish("error", "Error: the arguments are not a JSON object.");
    const ToolSpec * spec = find_tool(call.name);
    std::map<std::string, McpTool> mcp;
    bool offered = false;
    for (const ChatTool & t : tools_for(conv, run.remote, &mcp))
        offered = offered || t.name == call.name;
    const auto mcp_tool = mcp.find(call.name);
    const bool is_mcp = !spec && mcp_tool != mcp.end();
    if ((!spec && !is_mcp) || !offered)
        return finish("error", "Error: there is no tool " + call.name + " in this conversation.");
    if (is_mcp) item["mcp"] = mcp_tool->second.server;

    const std::string ws = workspace_path(conv);
    const std::string mode = conv.value("mode", "ask");
    bool needs_approval = false;
    if (is_mcp || spec->kind == Kind::Change) {
        const bool reads = call.name == "exec_shell_command" && is_read_only_command(arg(args, "command"));
        if (!reads && mode == "read_only")
            return finish("denied", "Refused: this conversation is read-only, so nothing may be changed. Say what you "
                                    "would change instead.");
        needs_approval = !reads && mode != "full";
    }

    // What an edit will write is computed before the card is shown, so the person approves the diff
    // that a yes applies.
    std::string target, content, diff, err;
    if (call.name == "write_file" || call.name == "edit_file") {
        if (!plan_change(ws, call.name, args, target, content, diff, err)) return finish("error", "Error: " + err);
        item["diff"] = diff;
        item["file"] = workspace_relative(ws, target);
    }
    if (needs_approval) {
        if (!wait_approval(run, conv_id, item)) {
            if (run.stop.load()) return finish("denied", "Not done: the run was stopped.");
            return finish("denied", run.auto_deny ? "Not done: this is a scheduled task and nobody is at the screen to "
                                                    "approve it. Say in your answer what you would have done."
                                                  : "The person declined this. Do not try it again; say what you "
                                                    "would have done instead.");
        }
        item["state"] = "running";
    }
    publish_item(conv_id, item, false);

    ToolOutput o;
    if (is_mcp) {
        std::string text;
        const bool ok = d_.mcp->call(mcp_tool->second.server, mcp_tool->second.name, args, text,
                                     [&run] { return run.stop.load(); });
        o = {ok, ok ? (text.empty() ? "(no output)" : text) : "Error: " + text, "", ""};
    } else if (call.name == "read_file")
        o = read_file(ws, args);
    else if (call.name == "file_glob_search")
        o = file_glob_search(ws, args);
    else if (call.name == "grep_search")
        o = grep_search(ws, args);
    else if (call.name == "get_info")
        o = {true, json{{"os", d_.os_name}, {"cwd", ws}}.dump(), "", ""};
    else if (call.name == "write_file" || call.name == "edit_file") {
        // Planned again: the file may have changed while the card waited.
        if (!plan_change(ws, call.name, args, target, content, diff, err) || !write_whole(target, content, err))
            o = fail(err);
        else {
            item["diff"] = diff;
            o = {true,
                 diff.empty()
                     ? "No change: the file already had that content."
                     : (call.name == "write_file" ? "File written successfully.\n" : "File edited successfully.\n") +
                           diff,
                 diff, workspace_relative(ws, target)};
        }
    } else if (call.name == "exec_shell_command") {
        const long asked_s = args.contains("timeout") && args["timeout"].is_number() ? args["timeout"].get<long>() : 0;
        const int timeout_s =
            asked_s > 0 ? (int) std::min<long>(asked_s, k_command_max_timeout_s) : k_command_default_timeout_s;
        const long asked_out = args.contains("max_output_size") && args["max_output_size"].is_number()
                                   ? args["max_output_size"].get<long>()
                                   : 0;
        const size_t max_out =
            asked_out > 0 ? std::min<size_t>((size_t) asked_out, k_tool_result_max_bytes) : k_tool_result_max_bytes;
        const platform::CommandResult r = d_.run_command(arg(args, "command"), ws, timeout_s, max_out, run.stop);
        if (!r.started)
            o = fail(r.error);
        else {
            // The output, then "[exit code: N]", as llama.cpp's exec_shell_command reports it. A failing
            // command is a result, not a tool error: the model reads why.
            std::string text =
                r.output + (r.truncated ? "\n[output cut at " + std::to_string(max_out) + " bytes]" : "");
            text += "\n[exit code: " + std::to_string(r.exit_code) + "]";
            if (r.timed_out) text += " [exit due to timed out after " + std::to_string(timeout_s) + " s]";
            if (r.stopped) text += " [stopped]";
            o = {!r.timed_out && !r.stopped, text, "", ""};
        }
    } else if (call.name == "use_skill") {
        o = fail("no switched-on skill is named " + arg(args, "name"));
        for (const auto & [s, on] : skills(ws))
            if (on && s.name == arg(args, "name")) o = {true, s.body, "", ""};
    }
    if (!o.file.empty()) item["file"] = o.file;
    if (!o.diff.empty()) item["diff"] = o.diff;
    return finish(o.ok ? "done" : "error", o.text);
}

// ---- the rounds ----

std::string Agent::run_rounds(Run & run, const std::string & conv_id) {
    json conv;
    if (!d_.store->conversation(conv_id, conv)) return "";
    // One system prompt for every round of this message: the engine reuses the prompt the KV holds
    // when the next request extends it, and the conversation is only ever appended to.
    const std::string system = system_prompt(conv, run.remote);
    bool told_shortened = false;
    // The last failed call (name, arguments, result) and how many times in a row it came back.
    std::string last_failure;
    int same_failures = 0;
    std::string final_text;
    int rounds = 0;
    for (;;) {
        if (run.stop.load()) {
            notice(conv_id, "info", "Stopped.");
            return final_text;
        }
        if (rounds >= k_max_rounds_per_message) {
            notice(conv_id, "warn",
                   "Stopped after " + std::to_string(k_max_rounds_per_message) +
                       " rounds of tool calls in one message. Send another message to let it go on.");
            return final_text;
        }
        d_.store->conversation(conv_id, conv);
        const ChatRequest req = build_request(conv, system, run.remote, run.think);

        json item{{"id", new_id("i")}, {"kind", "assistant"}, {"text", ""}, {"reasoning", ""}, {"streaming", true}};
        std::string sent_text, sent_reasoning;
        bool shown = false;
        const std::string item_id = item.value("id", "");
        const auto on_token = [&](const TokenMetrics & m) {
            const std::string text = m.text.substr(0, complete_utf8_prefix_size(m.text));
            const std::string reasoning = m.reasoning.substr(0, complete_utf8_prefix_size(m.reasoning));
            if (!shown) {
                // Shown at its first token: a round that fails before one leaves no empty bubble.
                shown = true;
                publish_item(conv_id, item, false);
            }
            if (starts_with(text, sent_text) && starts_with(reasoning, sent_reasoning)) {
                json d{{"conversation", conv_id}, {"item", item_id}};
                if (text.size() > sent_text.size()) d["text"] = text.substr(sent_text.size());
                if (reasoning.size() > sent_reasoning.size()) d["reasoning"] = reasoning.substr(sent_reasoning.size());
                if (d.size() > 2) d_.bus->publish("delta", d, audience_of(conv_id));
            } else {
                // The parser took back text it had shown (a closing reasoning tag): the item, whole.
                item["text"] = text;
                item["reasoning"] = reasoning;
                d_.bus->publish("item", json{{"conversation", conv_id}, {"item", item}}, audience_of(conv_id));
            }
            sent_text = text;
            sent_reasoning = reasoning;
            return !run.stop.load();
        };
        ChatOutcome out = ask_model(run, conv, req, on_token);
        if (out.status != 200) {
            if (shown) {
                item["text"] = sent_text;
                item["reasoning"] = sent_reasoning;
                item["streaming"] = false;
                publish_item(conv_id, item);
            }
            if (run.stop.load()) {
                notice(conv_id, "info", "Stopped.");
                return final_text;
            }
            if (out.code == "context_full") {
                // The oldest long tool result is cut to a few lines, for good: the next request is
                // the same prefix again from there on, so the engine still reuses what it can.
                bool cut = false;
                d_.store->edit_conversation(conv_id, [&](json & c) {
                    for (json & it : c["items"]) {
                        if (it.value("kind", "") == "tool" && !it.value("shortened", false) &&
                            it.value("result", "").size() > k_shortened_result_bytes) {
                            it["shortened"] = true;
                            cut = true;
                            return;
                        }
                    }
                });
                if (cut) {
                    if (!told_shortened)
                        notice(conv_id, "info",
                               "The conversation outgrew the model's context: older tool results were shortened "
                               "for the model (they are still shown here in full).");
                    told_shortened = true;
                    continue;
                }
                notice(conv_id, "error",
                       "This conversation no longer fits the model's context, even with every tool result shortened. "
                       "Start a new conversation, or load the model with a larger context.");
            } else if (out.code == error_code::no_model) {
                notice(conv_id, "error",
                       "No model is loaded. Load one from the Models page, then send the message again.");
            } else {
                notice(conv_id, "error", out.error.empty() ? "The model could not answer." : out.error);
            }
            run.failed = true;
            return final_text;
        }
        ++rounds;
        RunResult & r = out.result;
        json calls = json::array();
        for (size_t i = 0; i < r.tool_calls.size(); ++i) {
            if (r.tool_calls[i].id.empty()) r.tool_calls[i].id = "call_" + item_id + "_" + std::to_string(i);
            calls.push_back(
                {{"id", r.tool_calls[i].id}, {"name", r.tool_calls[i].name}, {"arguments", r.tool_calls[i].arguments}});
        }
        item["text"] = r.generated_text;
        item["reasoning"] = r.reasoning_text;
        item["streaming"] = false;
        if (!calls.empty()) item["tool_calls"] = calls;
        publish_item(conv_id, item);
        if (r.cancelled || run.stop.load()) {
            notice(conv_id, "info", "Stopped.");
            return final_text.empty() ? r.generated_text : final_text;
        }
        final_text = r.generated_text;
        if (r.tool_calls.empty()) return final_text;
        for (const ChatToolCall & c : r.tool_calls) {
            if (run.stop.load()) break;
            d_.store->conversation(conv_id, conv);
            const json done = run_tool(run, conv, c);
            if (done.value("state", "") != "error") {
                last_failure.clear();
                same_failures = 0;
                continue;
            }
            const std::string failure =
                c.name + '\n' + done.value("args", json::object()).dump() + '\n' + done.value("result", "");
            same_failures = failure == last_failure ? same_failures + 1 : 1;
            last_failure = failure;
            if (same_failures >= k_max_same_failed_calls) {
                notice(conv_id, "error",
                       "Stopped: the model asked for the same thing " + std::to_string(same_failures) +
                           " times and it failed the same way each time. Rephrase the request, or check what the "
                           "conversation can reach (its folder and mode).");
                run.failed = true;
                return final_text;
            }
        }
    }
}

// ---- starting, stopping ----

Refusal Agent::start(const std::string & conversation,
                     const std::string & text,
                     bool think,
                     bool remote,
                     bool auto_deny,
                     const std::string & task_id,
                     json & item_out) {
    if (text.empty()) return {400, "the message is empty", "invalid_request"};
    json conv;
    if (!d_.store->conversation(conversation, conv)) return {404, "no such conversation", "not_found"};
    auto run = std::make_shared<Run>();
    run->remote = remote;
    run->auto_deny = auto_deny;
    run->think = think;
    run->root = conversation;
    run->task = task_id;
    std::vector<std::thread> reap;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (shutting_down_) return {503, "the server is stopping", "stopping"};
        if (runs_.count(conversation)) return {409, "this conversation is already running", "busy"};
        runs_[conversation] = run;
        ++active_;
        // Runs that ended since the last start: their threads are joined here, briefly, since each
        // has already returned or is returning.
        for (auto it = finished_.begin(); it != finished_.end();) {
            if (it->second->thread.joinable()) reap.push_back(std::move(it->second->thread));
            it = finished_.erase(it);
        }
    }
    for (std::thread & t : reap)
        t.join();
    item_out = json{{"id", new_id("i")}, {"kind", "user"}, {"text", text}};
    const json item = item_out;
    d_.store->edit_conversation(conversation, [&](json & c) {
        c["state"] = "idle";
        if (c.value("title", "").empty()) c["title"] = short_text(text, k_title_max_chars);
    });
    publish_item(conversation, item);
    publish_conversation(conversation);
    std::lock_guard<std::mutex> lk(run->m); // the thread member is set before the thread can be reaped
    run->thread = std::thread([this, run, conversation] {
        // Until start() has stored this thread in the run, nobody may find the run finished and
        // try to join it: start() holds run->m across the assignment.
        { std::lock_guard<std::mutex> wait_for_start(run->m); }
        try {
            json c;
            d_.store->conversation(conversation, c);
            if (c.value("agent", std::string(k_builtin_agent)) == k_builtin_agent)
                run_rounds(*run, conversation);
            else
                run_external(*run, conversation);
        } catch (const std::exception & e) {
            run->failed = true;
            notice(conversation, "error", std::string("The run failed: ") + e.what());
        }
        std::string outcome = run->stop.load() ? "stopped" : run->failed ? "error" : "done";
        if (run->denied > 0)
            notice(conversation, "warn",
                   std::to_string(run->denied) + " step(s) needed an approval and were not done: nobody is at the "
                                                 "screen for a scheduled task. Run it in full mode to allow them.");
        {
            std::lock_guard<std::mutex> lk2(m_);
            runs_.erase(conversation);
        }
        d_.store->edit_conversation(conversation, [&](json & c) { c["state"] = run->failed ? "error" : "idle"; });
        publish_conversation(conversation);
        if (!run->task.empty()) finish_task(run->task, conversation, outcome);
        {
            std::lock_guard<std::mutex> lk2(run->m);
            run->done = true;
        }
        run->cv.notify_all();
        std::lock_guard<std::mutex> lk2(m_);
        finished_.emplace_back(conversation, run);
        --active_;
        cv_.notify_all();
    });
    return {};
}

Refusal
Agent::send(const std::string & conversation, const std::string & text, bool think, bool remote, json & item_out) {
    return start(conversation, text, think, remote, false, "", item_out);
}

Refusal Agent::edit(const std::string & conversation,
                    const std::string & item,
                    const std::string & text,
                    bool think,
                    bool remote,
                    json & item_out) {
    if (running(conversation)) return {409, "this conversation is running: stop it first", "busy"};
    {
        json c;
        if (d_.store->conversation(conversation, c) &&
            c.value("agent", std::string(k_builtin_agent)) != k_builtin_agent)
            return {409, "another agent's session cannot be rewritten: send a new message, or start a new conversation",
                    "unsupported"};
    }
    bool found = false;
    if (!d_.store->edit_conversation(conversation, [&](json & c) {
            json kept = json::array();
            for (const json & it : c["items"]) {
                if (it.value("id", "") == item && it.value("kind", "") == "user") found = true;
                if (found) continue;
                kept.push_back(it);
            }
            if (found) c["items"] = kept;
        }))
        return {404, "no such conversation", "not_found"};
    if (!found) return {404, "no message of the person with that id", "not_found"};
    return send(conversation, text, think, remote, item_out);
}

bool Agent::stop(const std::string & conversation) {
    std::shared_ptr<Run> r;
    {
        std::lock_guard<std::mutex> lk(m_);
        const auto it = runs_.find(conversation);
        if (it != runs_.end()) r = it->second;
    }
    if (!r) return false;
    r->stop = true;
    std::unique_lock<std::mutex> lk(r->m);
    r->cv.notify_all();
    r->cv.wait_for(lk, std::chrono::seconds(k_stop_wait_s), [&] { return r->done; });
    return true;
}

Refusal Agent::remove_conversation(const std::string & id) {
    if (running(id)) return {409, "this conversation is running: stop it first", "busy"};
    json c;
    if (!d_.store->conversation(id, c)) return {404, "no such conversation", "not_found"};
    d_.store->remove_conversation(id);
    close_external(id);
    return {};
}

// ---- tasks ----

json Agent::task_view(const json & task) const {
    json t = task;
    Schedule s;
    std::string err;
    std::time_t next = 0;
    const json last = task.value("last_run", json(nullptr));
    const bool ran = last.is_object() && last.contains("at");
    if (task.value("enabled", true) && parse_schedule(task.value("schedule", json::object()), s, err))
        next = next_run(s, ran ? last.value("at", (long long) 0) : task.value("created", (long long) 0), ran);
    t["next_run"] = next ? json(next) : json(nullptr);
    std::lock_guard<std::mutex> lk(m_);
    t["running"] = running_tasks_.count(task.value("id", "")) > 0;
    return t;
}

Refusal Agent::run_task(const std::string & id) {
    json task;
    if (!d_.store->task(id, task)) return {404, "no such task", "not_found"};
    {
        std::lock_guard<std::mutex> lk(m_);
        if (running_tasks_.count(id)) return {409, "this task is already running", "busy"};
        running_tasks_.insert(id);
    }
    json fields{{"title", task.value("title", "Task")},
                {"workspace", task.value("workspace", json(nullptr))},
                {"mode", task.value("mode", "read_only")},
                {"route", task.value("route", "local")},
                {"task", id}};
    const json conv = d_.store->create_conversation(fields);
    const std::string cid = conv.value("id", "");
    task["last_run"] = json{{"at", now_s()}, {"conversation", cid}, {"outcome", "running"}};
    d_.store->put_task(task);
    json item;
    const Refusal r = start(cid, task.value("prompt", ""), false, false, true, id, item);
    if (!r.ok()) {
        {
            std::lock_guard<std::mutex> lk(m_);
            running_tasks_.erase(id);
        }
        notice(cid, "error", "The task could not start: " + r.error);
        task["last_run"]["outcome"] = "error";
        d_.store->put_task(task);
        d_.bus->publish("task", task_view(task), EventBus::Audience::LocalOnly);
        return r;
    }
    d_.bus->publish("task", task_view(task), EventBus::Audience::LocalOnly);
    return {};
}

void Agent::finish_task(const std::string & task_id, const std::string & conv_id, const std::string & outcome) {
    json task;
    {
        std::lock_guard<std::mutex> lk(m_);
        running_tasks_.erase(task_id);
    }
    if (!d_.store->task(task_id, task)) return;
    json last = task.value("last_run", json::object());
    if (last.value("conversation", "") == conv_id) last["outcome"] = outcome;
    task["last_run"] = last;
    if (task.value("schedule", json::object()).value("kind", "") == "once") task["enabled"] = false;
    d_.store->put_task(task);
    d_.bus->publish("task", task_view(task), EventBus::Audience::LocalOnly);
    tasks_changed();
}

void Agent::tasks_changed() {
    {
        std::lock_guard<std::mutex> lk(sched_m_);
        sched_wake_ = true;
    }
    sched_cv_.notify_all();
}

void Agent::scheduler_loop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(sched_m_);
            sched_cv_.wait_for(lk, std::chrono::seconds(k_scheduler_poll_s), [&] { return sched_wake_; });
            sched_wake_ = false;
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            if (shutting_down_) return;
        }
        const std::time_t now = now_s();
        for (const json & t : d_.store->tasks()) {
            const json v = task_view(t);
            if (!v.value("enabled", true) || v.value("running", false) || !v["next_run"].is_number()) continue;
            if (v["next_run"].get<long long>() <= now) run_task(v.value("id", ""));
        }
    }
}

} // namespace bmoe::server::agent
