#include "agent.h"

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

// One message's run: its stop, the approvals it waits for, and the helpers it started. Shared by
// the run's thread and the HTTP workers that approve or stop it; every conversation the run works
// on (the one the message went to, and each helper) maps to the same Run.
struct Agent::Run {
    std::atomic<bool> stop{false};
    std::mutex m;
    std::condition_variable cv;
    std::map<std::string, int> decisions;          // item -> -1 waiting, 0 denied, 1 approved; under m
    std::map<std::string, std::string> waiting_in; // item -> the conversation it waits in; under m
    bool done = false;                             // under m
    bool remote = false;
    bool auto_deny = false;
    bool think = false;
    bool failed = false; // run thread only
    int helpers = 0;     // run thread only
    int denied = 0;      // run thread only
    std::string root;
    std::string task;
    std::thread thread;
};

namespace {

// Tool names, the categories the mode rules apply to, and the words of a tool item's title.
enum class Kind { Read, Change, Web, Skill, Delegate };

struct ToolSpec {
    const char * name;
    Kind kind;
    const char * description;
    const char * parameters;
};

// The tools as the model reads them. Every schema is an object with its required fields; the
// descriptions say what the tool cannot do, so a model does not try it.
const ToolSpec kTools[] = {
    {"list_files", Kind::Read, "List a folder of the workspace: its files with sizes, and its subfolders.",
     R"({"type":"object","properties":{"path":{"type":"string","description":"Folder, relative to the workspace. Default: the workspace itself."}}})"},
    {"read_file", Kind::Read,
     "Read a text file of the workspace, whole or a range of lines. Long files are cut; ask for a line range then.",
     R"({"type":"object","properties":{"path":{"type":"string"},"start_line":{"type":"integer","description":"First line, from 1."},"end_line":{"type":"integer","description":"Last line, included."}},"required":["path"]})"},
    {"search_files", Kind::Read,
     "Find the lines that contain a text (case-insensitive, not a regular expression) in the files under a folder.",
     R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string","description":"Folder to search, relative to the workspace. Default: all of it."}},"required":["pattern"]})"},
    {"write_file", Kind::Change, "Create a file, or replace a file's whole content. Folders are created as needed.",
     R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})"},
    {"edit_file", Kind::Change,
     "Replace one exact passage of a file with new text. The passage must appear exactly once: include enough "
     "surrounding lines to make it unique.",
     R"({"type":"object","properties":{"path":{"type":"string"},"old_text":{"type":"string"},"new_text":{"type":"string"}},"required":["path","old_text","new_text"]})"},
    {"run_command", Kind::Change,
     "Run a shell command in the workspace folder, with a time limit; stdout and stderr come back, cut if long. "
     "No input can be typed into it.",
     R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})"},
    {"fetch_url", Kind::Web, "Read a web page (https) as plain text.",
     R"({"type":"object","properties":{"url":{"type":"string"}},"required":["url"]})"},
    {"web_search", Kind::Web, "Search the web: result titles, links and snippets. Read a result with fetch_url.",
     R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})"},
    {"use_skill", Kind::Skill, "Read the instructions of one of the skills listed in the system prompt.",
     R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})"},
    {"delegate", Kind::Delegate,
     "Hand a self-contained part of the job to a helper, which works on it with the same tools and returns its "
     "final answer. Say everything it needs in `task`: it does not see this conversation.",
     R"({"type":"object","properties":{"task":{"type":"string"},"title":{"type":"string","description":"A few words for the person."}},"required":["task"]})"},
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
    if (tool == "list_files") return "List " + (arg(a, "path").empty() ? std::string(".") : arg(a, "path"));
    if (tool == "read_file") return "Read " + arg(a, "path");
    if (tool == "search_files") return "Search for \"" + short_text(arg(a, "pattern"), 40) + "\"";
    if (tool == "write_file") return "Write " + arg(a, "path");
    if (tool == "edit_file") return "Edit " + arg(a, "path");
    if (tool == "run_command") return "Run " + short_text(arg(a, "command"), 60);
    if (tool == "fetch_url") return "Read " + short_text(arg(a, "url"), 80);
    if (tool == "web_search") return "Search the web for \"" + short_text(arg(a, "query"), 50) + "\"";
    if (tool == "use_skill") return "Use the skill " + arg(a, "name");
    if (tool == "delegate")
        return "Helper: " + (arg(a, "title").empty() ? short_text(arg(a, "task"), 50) : arg(a, "title"));
    return tool;
}

bool read_whole(const std::string & path, std::string & out, std::string & err) {
    std::error_code ec;
    const auto size = fs::file_size(fs::u8path(path), ec);
    if (ec) {
        err = "cannot read the file (" + ec.message() + ")";
        return false;
    }
    if (size > k_read_max_file_bytes) {
        err = "the file is " + std::to_string(size) + " bytes, more than the " + std::to_string(k_read_max_file_bytes) +
              " a tool reads";
        return false;
    }
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (!f && !f.eof()) {
        err = "cannot read the file";
        return false;
    }
    return true;
}

bool write_whole(const std::string & path, const std::string & content, std::string & err) {
    std::error_code ec;
    fs::create_directories(fs::u8path(path).parent_path(), ec);
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
    f << content;
    if (!f) {
        err = "cannot write the file";
        return false;
    }
    return true;
}

bool skip_dir(const fs::path & p) {
    const std::string n = p.filename().u8string();
    for (const char * s : k_search_skip_dirs)
        if (n == s) return true;
    return false;
}

std::string lower(std::string s) {
    for (char & c : s)
        c = (char) std::tolower((unsigned char) c);
    return s;
}

// What a tool did: the text the model reads, and for an edit, the diff and the file a UI previews.
struct ToolOutput {
    bool ok = true;
    std::string text;
    std::string diff;
    std::string file;
};

ToolOutput fail(const std::string & why) {
    return {false, "Error: " + why, "", ""};
}

ToolOutput list_files(const std::string & ws, const json & a) {
    std::string dir, err;
    if (!resolve_in_workspace(ws, arg(a, "path"), dir, err)) return fail(err);
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(dir), ec)) return fail(arg(a, "path") + " is not a folder");
    std::vector<std::string> dirs, files;
    for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().u8string();
        if (it->is_directory(ec))
            dirs.push_back(n + "/");
        else
            files.push_back(n + "  (" + std::to_string(it->file_size(ec)) + " bytes)");
    }
    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());
    std::string out;
    size_t shown = 0;
    for (const auto * list : {&dirs, &files})
        for (const std::string & e : *list) {
            if (shown++ >= k_list_max_entries) break;
            out += e + "\n";
        }
    const size_t total = dirs.size() + files.size();
    if (total > k_list_max_entries)
        out += "[" + std::to_string(total - k_list_max_entries) + " more entries not shown]\n";
    if (total == 0) out = "(empty folder)\n";
    return {true, out, "", ""};
}

ToolOutput read_file(const std::string & ws, const json & a) {
    std::string path, err, text;
    if (!resolve_in_workspace(ws, arg(a, "path"), path, err)) return fail(err);
    if (!read_whole(path, text, err)) return fail(err);
    if (looks_binary(text)) return fail(arg(a, "path") + " is not a text file");
    const long first =
        a.contains("start_line") && a["start_line"].is_number_integer() ? a["start_line"].get<long>() : 1;
    const long last = a.contains("end_line") && a["end_line"].is_number_integer() ? a["end_line"].get<long>() : 0;
    if (first <= 1 && last <= 0) return {true, text.empty() ? "(empty file)" : text, "", workspace_relative(ws, path)};
    std::istringstream in(text);
    std::string line, out;
    long n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (n >= first && (last <= 0 || n <= last)) out += line + "\n";
    }
    return {true,
            "[lines " + std::to_string(first) + "-" + std::to_string(last > 0 ? std::min(last, n) : n) + " of " +
                std::to_string(n) + "]\n" + out,
            "", workspace_relative(ws, path)};
}

ToolOutput search_files(const std::string & ws, const json & a) {
    const std::string pattern = lower(arg(a, "pattern"));
    if (pattern.empty()) return fail("pattern is empty");
    std::string dir, err;
    if (!resolve_in_workspace(ws, arg(a, "path"), dir, err)) return fail(err);
    std::string out;
    size_t hits = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(fs::u8path(dir), fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && hits < k_search_max_hits; it.increment(ec)) {
        if (it->is_directory(ec)) {
            if (skip_dir(it->path())) it.disable_recursion_pending();
            continue;
        }
        // A link may lead out of the workspace; search only what resolves inside it.
        std::string real;
        if (it->is_symlink(ec) && !resolve_in_workspace(ws, it->path().u8string(), real, err)) continue;
        if (!it->is_regular_file(ec) || it->file_size(ec) > k_search_max_file_bytes) continue;
        std::string text;
        if (!read_whole(it->path().u8string(), text, err) || looks_binary(text)) continue;
        std::istringstream in(text);
        std::string line;
        long n = 0;
        const std::string rel = workspace_relative(ws, it->path().u8string());
        while (std::getline(in, line) && hits < k_search_max_hits) {
            ++n;
            if (lower(line).find(pattern) == std::string::npos) continue;
            out += rel + ":" + std::to_string(n) + ": " + short_text(line, 300) + "\n";
            ++hits;
        }
    }
    if (hits == 0) return {true, "No matches.", "", ""};
    if (hits >= k_search_max_hits) out += "[stopped at " + std::to_string(k_search_max_hits) + " matches]\n";
    return {true, out, "", ""};
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
        content = arg(a, "content");
    } else {
        if (!exists) {
            err = arg(a, "path") + " does not exist";
            return false;
        }
        const std::string old_text = arg(a, "old_text");
        if (old_text.empty()) {
            err = "old_text is empty";
            return false;
        }
        size_t count = 0, at = 0, first = std::string::npos;
        while ((at = before.find(old_text, at)) != std::string::npos) {
            if (first == std::string::npos) first = at;
            ++count;
            at += old_text.size();
        }
        if (count == 0) {
            err = "the passage was not found in " + arg(a, "path") + ": read the file again and copy it exactly";
            return false;
        }
        if (count > 1) {
            err = "the passage appears " + std::to_string(count) + " times: include more surrounding lines";
            return false;
        }
        content = before.substr(0, first) + arg(a, "new_text") + before.substr(first + old_text.size());
    }
    diff = unified_diff(workspace_relative(ws, path), before, content);
    return true;
}

ToolOutput fetch_url(HttpFetch * net, const json & a, const std::atomic<bool> & stop) {
    const std::string url = arg(a, "url");
    if (!net) return fail("this server cannot make web requests");
    if (url_names_private_host(url))
        return fail("the agent reads the public web only: " + url + " is this computer or a private network");
    HttpFetch::Request r;
    r.url = url;
    r.timeout_s = k_web_timeout_s;
    r.accept = "text/html,text/plain,application/json;q=0.9,*/*;q=0.5";
    std::string body;
    bool cut = false;
    int status = 0;
    std::string err;
    const bool ok = net->stream(
        r,
        [&](const char * d, size_t n) {
            const size_t room = k_web_max_page_bytes - body.size();
            body.append(d, std::min(n, room));
            if (n >= room) cut = true;
            return !cut && !stop.load();
        },
        status, err);
    if (!ok) return fail(err);
    if (stop.load()) return fail("stopped");
    if (status >= 400) return fail("the page answered HTTP " + std::to_string(status));
    const std::string low = lower(body.substr(0, 2048));
    const bool html = low.find("<html") != std::string::npos || low.find("<!doctype html") != std::string::npos ||
                      low.find("<body") != std::string::npos || low.find("<head") != std::string::npos;
    std::string text = html ? html_to_text(body) : body;
    if (looks_binary(text)) return fail("the page is not text");
    if (cut) text += "\n[the page was cut at " + std::to_string(k_web_max_page_bytes) + " bytes]";
    return {true, text, "", ""};
}

ToolOutput web_search(HttpFetch * net, const json & a, const std::atomic<bool> & stop) {
    const std::string q = arg(a, "query");
    if (q.empty()) return fail("query is empty");
    if (!net) return fail("this server cannot make web requests");
    HttpFetch::Request r;
    r.method = "POST";
    r.url = k_web_search_endpoint;
    r.body = "q=" + url_encode(q);
    r.content_type = "application/x-www-form-urlencoded";
    r.timeout_s = k_web_timeout_s;
    std::string body, err;
    int status = 0;
    if (!net->stream(
            r,
            [&](const char * d, size_t n) {
                body.append(d, std::min(n, k_web_max_page_bytes - body.size()));
                return body.size() < k_web_max_page_bytes && !stop.load();
            },
            status, err))
        return fail(err);
    if (status >= 400) return fail("the search answered HTTP " + std::to_string(status));
    const std::vector<SearchHit> hits = parse_search_results(body);
    if (hits.empty()) return {true, "No results (or the search page could not be read).", "", ""};
    std::string out;
    for (size_t i = 0; i < hits.size(); ++i)
        out += std::to_string(i + 1) + ". " + hits[i].title + "\n   " + hits[i].url + "\n   " + hits[i].snippet + "\n";
    return {true, out, "", ""};
}

std::string today() {
    const std::time_t t = std::time(nullptr);
    char buf[64];
    std::tm tm{};
    static std::mutex m;
    {
        std::lock_guard<std::mutex> lk(m);
        tm = *std::localtime(&t);
    }
    std::strftime(buf, sizeof(buf), "%A %Y-%m-%d", &tm);
    return buf;
}

const char * mode_words(const std::string & mode) {
    if (mode == "full") return "Mode: full. You may change files, run commands and use the web without asking.";
    if (mode == "read_only")
        return "Mode: read-only. You may read, but every change to a file and every command that could change "
               "something is refused. Say what you would change instead of trying it.";
    return "Mode: ask. Reading needs no permission. A change to a file, a command that could change something, and "
           "the first use of the web wait for the person's approval. If the person declines, do not try the same "
           "thing again: say what you would have done.";
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
    for (const auto & [item, conv] : r->waiting_in)
        if (conv == id || id == r->root) return "waiting";
    return "running";
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
                         {"updated", c.value("updated", 0LL)},
                         {"parent", c.value("parent", json(nullptr))}},
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
    const std::string ws = workspace_path(conv);
    std::string s = "You are the agent of BigMoeOnEdge, working for the person at this computer. Today is " + today() +
                    ". The computer runs " + d_.os_name + ".\n";
    if (!ws.empty() && !remote) {
        json w;
        d_.store->workspace(conv.value("workspace", ""), w);
        s += "Workspace: \"" + w.value("name", "") + "\" at " + ws +
             ". Every path you give a tool is relative to it, and nothing outside it can be reached.\n";
    } else {
        s += "This conversation has no workspace: there are no file or command tools.\n";
    }
    s += mode_words(conv.value("mode", "ask"));
    s += "\n";
    bool any = false;
    for (const auto & [skill, enabled] : skills(ws)) {
        if (!enabled) continue;
        if (!any) s += "Skills you can follow (read one with use_skill when the task matches its description):\n";
        any = true;
        s += "- " + skill.name + ": " + skill.description + "\n";
    }
    s += "Work step by step with the tools, check what you change, and end with a short answer that says what you "
         "did and what is left.";
    return s;
}

std::vector<ChatTool> Agent::tools_for(const json & conv, bool remote, int depth) const {
    const std::string ws = workspace_path(conv);
    const std::string mode = conv.value("mode", "ask");
    bool any_skill = false;
    for (const auto & [s, on] : skills(ws))
        any_skill = any_skill || on;
    std::vector<ChatTool> out;
    for (const ToolSpec & t : kTools) {
        const std::string n = t.name;
        bool offered = false;
        switch (t.kind) {
        case Kind::Read:
            offered = !ws.empty() && !remote;
            break;
        case Kind::Change:
            // A change in read-only mode could only be refused, so it is not offered; a command still
            // is, since one that only reads runs.
            offered = !ws.empty() && !remote && (n == "run_command" ? d_.can_run_commands : mode != "read_only");
            break;
        case Kind::Web:
            offered = d_.net != nullptr && !remote;
            break;
        case Kind::Skill:
            offered = any_skill;
            break;
        case Kind::Delegate:
            offered = depth < k_max_delegate_depth;
            break;
        }
        if (offered) out.push_back({t.name, t.description, t.parameters});
    }
    return out;
}

ChatRequest
Agent::build_request(const json & conv, const std::string & system, bool remote, int depth, bool think) const {
    ChatRequest req;
    req.messages.push_back({"system", system});
    const json & items = conv["items"];
    // What each call got back, by its id: a tool item's result, or a helper's summary.
    std::map<std::string, std::string> answers;
    for (const json & it : items) {
        const std::string call = it.value("call_id", "");
        if (call.empty()) continue;
        std::string r = it.value("kind", "") == "helper" ? it.value("summary", "") : it.value("result", "");
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
    req.tools = tools_for(conv, remote, depth);
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
    if (conv_id != run.root) publish_conversation(run.root);
    int decision = 0;
    {
        std::unique_lock<std::mutex> lk(run.m);
        run.cv.wait(lk, [&] { return run.decisions[id] != -1 || run.stop.load(); });
        decision = run.decisions[id] == 1 && !run.stop.load() ? 1 : 0;
        run.decisions.erase(id);
        run.waiting_in.erase(id);
    }
    publish_conversation(conv_id);
    if (conv_id != run.root) publish_conversation(run.root);
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

json Agent::run_tool(Run & run, const json & conv, const ChatToolCall & call, int depth) {
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
    bool offered = false;
    for (const ChatTool & t : tools_for(conv, run.remote, depth))
        offered = offered || t.name == call.name;
    if (!spec || !offered) return finish("error", "Error: there is no tool " + call.name + " in this conversation.");

    const std::string ws = workspace_path(conv);
    const std::string mode = conv.value("mode", "ask");
    bool needs_approval = false;
    if (spec->kind == Kind::Change) {
        const bool reads = call.name == "run_command" && is_read_only_command(arg(args, "command"));
        if (!reads && mode == "read_only")
            return finish("denied", "Refused: this conversation is read-only, so nothing may be changed. Say what you "
                                    "would change instead.");
        needs_approval = !reads && mode != "full";
    } else if (spec->kind == Kind::Web) {
        needs_approval = mode != "full" && !conv.value("web_allowed", false);
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
        if (spec->kind == Kind::Web) d_.store->edit_conversation(conv_id, [](json & c) { c["web_allowed"] = true; });
        item["state"] = "running";
        publish_item(conv_id, item, false);
    } else {
        publish_item(conv_id, item, false);
    }

    ToolOutput o;
    if (call.name == "list_files")
        o = list_files(ws, args);
    else if (call.name == "read_file")
        o = read_file(ws, args);
    else if (call.name == "search_files")
        o = search_files(ws, args);
    else if (call.name == "write_file" || call.name == "edit_file") {
        // Planned again: the file may have changed while the card waited.
        if (!plan_change(ws, call.name, args, target, content, diff, err) || !write_whole(target, content, err))
            o = fail(err);
        else {
            item["diff"] = diff;
            o = {true, diff.empty() ? "No change: the file already had that content." : "Done.\n" + diff, diff,
                 workspace_relative(ws, target)};
        }
    } else if (call.name == "run_command") {
        const platform::CommandResult r =
            d_.run_command(arg(args, "command"), ws, k_command_timeout_s, k_tool_result_max_bytes, run.stop);
        if (!r.started)
            o = fail(r.error);
        else {
            std::string head = r.timed_out
                                   ? "Ended after the " + std::to_string(k_command_timeout_s) + " s time limit.\n"
                               : r.stopped ? "Stopped.\n"
                                           : "Exit code " + std::to_string(r.exit_code) + ".\n";
            o = {r.exit_code == 0,
                 head + (r.output.empty() ? "(no output)" : r.output) +
                     (r.truncated ? "\n[output cut at " + std::to_string(k_tool_result_max_bytes) + " bytes]" : ""),
                 "", ""};
            if (!o.ok && !r.timed_out && !r.stopped) o.ok = true; // a failing command is a result, not a tool error
        }
    } else if (call.name == "fetch_url")
        o = fetch_url(d_.net, args, run.stop);
    else if (call.name == "web_search")
        o = web_search(d_.net, args, run.stop);
    else if (call.name == "use_skill") {
        o = fail("no switched-on skill is named " + arg(args, "name"));
        for (const auto & [s, on] : skills(ws))
            if (on && s.name == arg(args, "name")) o = {true, s.body, "", ""};
    } else if (call.name == "delegate") {
        const std::string task = arg(args, "task");
        if (task.empty()) return finish("error", "Error: task is empty.");
        if (run.helpers >= k_max_helpers_per_message)
            return finish("error", "Error: this message already started " + std::to_string(k_max_helpers_per_message) +
                                       " helpers; do the rest yourself.");
        ++run.helpers;
        json h{{"workspace", conv.value("workspace", json(nullptr))},
               {"mode", mode},
               {"route", conv.value("route", "local")},
               {"title", item.value("title", "Helper")},
               {"parent", conv_id}};
        const json helper = d_.store->create_conversation(h);
        const std::string hid = helper.value("id", "");
        json hitem{{"id", item.value("id", "")}, {"kind", "helper"}, {"title", item.value("title", "")},
                   {"state", "running"},         {"summary", ""},    {"conversation", hid},
                   {"call_id", call.id}};
        {
            std::lock_guard<std::mutex> lk(m_);
            runs_[hid] = runs_[run.root];
        }
        publish_item(conv_id, hitem);
        publish_item(hid, json{{"id", new_id("i")}, {"kind", "user"}, {"text", task}});
        publish_conversation(hid);
        const bool failed_before = run.failed;
        std::string answer = run_rounds(run, hid, depth + 1);
        const bool helper_failed = run.failed && !failed_before;
        run.failed = failed_before;
        {
            std::lock_guard<std::mutex> lk(m_);
            runs_.erase(hid);
        }
        d_.store->edit_conversation(hid, [&](json & c) { c["state"] = helper_failed ? "error" : "idle"; });
        publish_conversation(hid);
        if (answer.empty())
            answer = run.stop.load() ? "The helper was stopped." : "The helper ended without an answer.";
        hitem["state"] = run.stop.load() || helper_failed ? "error" : "done";
        hitem["summary"] = cap_text(answer, k_tool_result_max_bytes);
        publish_item(conv_id, hitem);
        return hitem;
    }
    if (!o.file.empty()) item["file"] = o.file;
    if (!o.diff.empty()) item["diff"] = o.diff;
    return finish(o.ok ? "done" : "error", o.text);
}

// ---- the rounds ----

std::string Agent::run_rounds(Run & run, const std::string & conv_id, int depth) {
    json conv;
    if (!d_.store->conversation(conv_id, conv)) return "";
    // One system prompt for every round of this message: the engine reuses the prompt the KV holds
    // when the next request extends it, and the conversation is only ever appended to.
    const std::string system = system_prompt(conv, run.remote);
    bool told_shortened = false;
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
        const ChatRequest req = build_request(conv, system, run.remote, depth, run.think);

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
                        const std::string k = it.value("kind", "");
                        const std::string r = k == "helper" ? it.value("summary", "") : it.value("result", "");
                        if ((k == "tool" || k == "helper") && !it.value("shortened", false) &&
                            r.size() > k_shortened_result_bytes) {
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
            run_tool(run, conv, c, depth);
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
    if (!conv.value("parent", json(nullptr)).is_null())
        return {409, "a helper's conversation takes no messages: it runs inside its parent's", "busy"};
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
            run_rounds(*run, conversation, 0);
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
    for (const json & it : c["items"])
        if (it.value("kind", "") == "helper") d_.store->remove_conversation(it.value("conversation", ""));
    d_.store->remove_conversation(id);
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
