#include "agent_store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace bmoe::server::agent {

std::time_t now_s() {
    return std::time(nullptr);
}

std::string new_id(const char * prefix) {
    static std::mutex m;
    static std::mt19937_64 rng{std::random_device{}()};
    static std::atomic<unsigned> seq{0};
    uint64_t r;
    {
        std::lock_guard<std::mutex> lk(m);
        r = rng();
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%010llx%04x%08llx", prefix, (unsigned long long) now_s(),
                  seq.fetch_add(1) & 0xffff, (unsigned long long) (r & 0xffffffffull));
    return buf;
}

bool write_file_atomic(const std::string & path, const std::string & text, bool owner_only, std::string & err) {
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.parent_path() / (p.filename().u8string() + ".tmp");
    fs::remove(tmp, ec);
    if (owner_only) {
        // Created empty and narrowed before the secret is written, so there is no moment the bytes
        // sit in a file another user may read.
        { std::ofstream create(tmp, std::ios::binary | std::ios::trunc); }
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    }
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        f.flush();
        if (!f) {
            err = "could not write " + tmp.u8string();
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        err = "could not replace " + p.u8string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

namespace {

bool read_json(const fs::path & p, json & out, std::vector<std::string> & warnings) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return false;
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    try {
        out = json::parse(ss.str());
        return true;
    } catch (const std::exception & e) {
        warnings.push_back(p.u8string() + " does not parse (" + e.what() + "); set aside as .bad");
        fs::rename(p, fs::u8path(p.u8string() + ".bad"), ec);
        return false;
    }
}

json find_by_id(const json & arr, const std::string & id) {
    for (const json & e : arr)
        if (e.value("id", "") == id) return e;
    return nullptr;
}

bool erase_by_id(json & arr, const std::string & id) {
    for (auto it = arr.begin(); it != arr.end(); ++it)
        if (it->value("id", "") == id) {
            arr.erase(it);
            return true;
        }
    return false;
}

json summary(const json & c) {
    json s = c;
    s.erase("items");
    return s;
}

} // namespace

AgentStore::AgentStore(std::string data_dir) : data_dir_(std::move(data_dir)) {}

std::string AgentStore::dir() const {
    return (fs::u8path(data_dir_) / "agent").u8string();
}

std::vector<std::string> AgentStore::load() {
    std::vector<std::string> warnings;
    std::lock_guard<std::mutex> lk(m_);
    const fs::path d = fs::u8path(dir());
    json j;
    if (read_json(d / "workspaces.json", j, warnings) && j.is_array()) workspaces_ = j;
    if (read_json(d / "tasks.json", j, warnings) && j.is_array()) tasks_ = j;
    if (read_json(d / "providers.json", j, warnings) && j.is_array()) providers_ = j;
    if (read_json(d / "skills.json", j, warnings) && j.is_object())
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.value().is_boolean()) skills_[it.key()] = it.value().get<bool>();
    if (read_json(fs::u8path(data_dir_) / "credentials.json", j, warnings) && j.is_object())
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.value().is_string()) keys_[it.key()] = it.value().get<std::string>();
    std::error_code ec;
    for (fs::directory_iterator it(d / "conversations", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        json c;
        if (!read_json(it->path(), c, warnings) || !c.is_object() || !c.contains("id") || !c["id"].is_string())
            continue;
        if (!c.contains("items") || !c["items"].is_array()) c["items"] = json::array();
        const std::string st = c.value("state", "idle");
        // A run lives in this process: one that was going when the server stopped did not finish.
        if (st == "running" || st == "waiting") {
            c["state"] = "idle";
            for (json & item : c["items"]) {
                if (item.value("streaming", false)) item["streaming"] = false;
                const std::string is = item.value("state", "");
                if (is == "running" || is == "waiting") item["state"] = "error";
            }
            c["items"].push_back(
                {{"id", new_id("i")},
                 {"kind", "notice"},
                 {"level", "warn"},
                 {"text", "The server stopped while this conversation was running; its run ended there."}});
            save_conversation_locked(c);
        }
        conversations_[c["id"].get<std::string>()] = c;
    }
    return warnings;
}

void AgentStore::save_conversation_locked(const json & c) const {
    std::string err;
    const fs::path p = fs::u8path(dir()) / "conversations" / (c.value("id", "") + ".json");
    if (!write_file_atomic(p.u8string(), c.dump(-1, ' ', false, json::error_handler_t::replace), false, err))
        std::fprintf(stderr, "warning: %s\n", err.c_str());
}

void AgentStore::save_list_locked(const std::string & file, const json & j) const {
    std::string err;
    if (!write_file_atomic((fs::u8path(dir()) / file).u8string(), j.dump(2, ' ', false, json::error_handler_t::replace),
                           false, err))
        std::fprintf(stderr, "warning: %s\n", err.c_str());
}

bool AgentStore::save_credentials_locked(std::string & err) const {
    json j = json::object();
    for (const auto & [id, key] : keys_)
        j[id] = key;
    return write_file_atomic((fs::u8path(data_dir_) / "credentials.json").u8string(),
                             j.dump(2, ' ', false, json::error_handler_t::replace), true, err);
}

json AgentStore::list_conversations() const {
    std::vector<json> out;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (const auto & [id, c] : conversations_)
            if (c.value("parent", json(nullptr)).is_null()) out.push_back(summary(c));
    }
    std::sort(out.begin(), out.end(),
              [](const json & a, const json & b) { return a.value("updated", 0LL) > b.value("updated", 0LL); });
    json arr = json::array();
    for (json & s : out)
        arr.push_back(std::move(s));
    return arr;
}

bool conversation_reaches(const json & conv) {
    return conv.value("workspace", json(nullptr)).is_string() || conv.value("route", "local") != "local" ||
           conv.value("agent", "builtin") != "builtin";
}

bool network_may_not_set(const json & fields) {
    return (fields.contains("workspace") && !fields["workspace"].is_null()) ||
           (fields.contains("route") && fields["route"] != "local") ||
           (fields.contains("mode") && fields["mode"] == "full") ||
           (fields.contains("agent") && fields["agent"] != "builtin");
}

bool AgentStore::reaches(const std::string & id) const {
    std::lock_guard<std::mutex> lk(m_);
    const auto it = conversations_.find(id);
    return it != conversations_.end() && conversation_reaches(it->second);
}

bool AgentStore::conversation(const std::string & id, json & out) const {
    std::lock_guard<std::mutex> lk(m_);
    const auto it = conversations_.find(id);
    if (it == conversations_.end()) return false;
    out = it->second;
    return true;
}

json AgentStore::create_conversation(json fields) {
    const std::time_t t = now_s();
    json c{{"id", new_id("c")},
           {"title", fields.value("title", std::string())},
           {"workspace", fields.value("workspace", json(nullptr))},
           {"mode", fields.value("mode", std::string("ask"))},
           {"route", fields.value("route", std::string("local"))},
           {"agent", fields.value("agent", std::string("builtin"))},
           {"state", "idle"},
           {"created", t},
           {"updated", t},
           {"task", fields.value("task", json(nullptr))},
           {"parent", fields.value("parent", json(nullptr))},
           {"web_allowed", false},
           {"items", fields.contains("items") && fields["items"].is_array() ? fields["items"] : json::array()}};
    std::lock_guard<std::mutex> lk(m_);
    conversations_[c["id"].get<std::string>()] = c;
    save_conversation_locked(c);
    return c;
}

bool AgentStore::edit_conversation(const std::string & id,
                                   const std::function<void(json &)> & edit,
                                   bool persist,
                                   json * after) {
    std::lock_guard<std::mutex> lk(m_);
    const auto it = conversations_.find(id);
    if (it == conversations_.end()) return false;
    edit(it->second);
    it->second["updated"] = now_s();
    if (persist) save_conversation_locked(it->second);
    if (after) *after = it->second;
    return true;
}

bool AgentStore::remove_conversation(const std::string & id) {
    std::lock_guard<std::mutex> lk(m_);
    if (!conversations_.erase(id)) return false;
    std::error_code ec;
    fs::remove(fs::u8path(dir()) / "conversations" / (id + ".json"), ec);
    return true;
}

bool AgentStore::upsert_item(const std::string & conversation, const json & item, bool persist) {
    return edit_conversation(
        conversation,
        [&](json & c) {
            const std::string id = item.value("id", "");
            for (json & existing : c["items"])
                if (existing.value("id", "") == id) {
                    existing = item;
                    return;
                }
            c["items"].push_back(item);
        },
        persist);
}

bool AgentStore::remove_item(const std::string & conversation, const std::string & item) {
    return edit_conversation(conversation, [&](json & c) { erase_by_id(c["items"], item); });
}

json AgentStore::workspaces() const {
    std::lock_guard<std::mutex> lk(m_);
    return workspaces_;
}

bool AgentStore::workspace(const std::string & id, json & out) const {
    std::lock_guard<std::mutex> lk(m_);
    out = find_by_id(workspaces_, id);
    return !out.is_null();
}

bool AgentStore::add_workspace(const std::string & path, const std::string & name, json & out, std::string & err) {
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    if (path.empty() || !p.is_absolute() || !fs::is_directory(p, ec)) {
        err = "not a folder: " + path + " (give the absolute path of an existing folder)";
        return false;
    }
    const std::string real = fs::canonical(p, ec).u8string();
    if (ec) {
        err = "cannot resolve " + path;
        return false;
    }
    std::lock_guard<std::mutex> lk(m_);
    for (const json & w : workspaces_)
        if (w.value("path", "") == real) {
            out = w;
            return true;
        }
    std::string n = name;
    if (n.empty()) n = fs::u8path(real).filename().u8string();
    if (n.empty()) n = real;
    out = json{{"id", new_id("w")}, {"name", n}, {"path", real}};
    workspaces_.push_back(out);
    save_list_locked("workspaces.json", workspaces_);
    return true;
}

bool AgentStore::remove_workspace(const std::string & id) {
    std::lock_guard<std::mutex> lk(m_);
    if (!erase_by_id(workspaces_, id)) return false;
    save_list_locked("workspaces.json", workspaces_);
    return true;
}

std::map<std::string, bool> AgentStore::skill_switches() const {
    std::lock_guard<std::mutex> lk(m_);
    return skills_;
}

void AgentStore::set_skill_enabled(const std::string & name, bool enabled) {
    std::lock_guard<std::mutex> lk(m_);
    skills_[name] = enabled;
    json j = json::object();
    for (const auto & [n, on] : skills_)
        j[n] = on;
    save_list_locked("skills.json", j);
}

json AgentStore::tasks() const {
    std::lock_guard<std::mutex> lk(m_);
    return tasks_;
}

bool AgentStore::task(const std::string & id, json & out) const {
    std::lock_guard<std::mutex> lk(m_);
    out = find_by_id(tasks_, id);
    return !out.is_null();
}

void AgentStore::put_task(const json & task) {
    std::lock_guard<std::mutex> lk(m_);
    const std::string id = task.value("id", "");
    bool replaced = false;
    for (json & t : tasks_)
        if (t.value("id", "") == id) {
            t = task;
            replaced = true;
        }
    if (!replaced) tasks_.push_back(task);
    save_list_locked("tasks.json", tasks_);
}

bool AgentStore::remove_task(const std::string & id) {
    std::lock_guard<std::mutex> lk(m_);
    if (!erase_by_id(tasks_, id)) return false;
    save_list_locked("tasks.json", tasks_);
    return true;
}

json AgentStore::providers() const {
    std::lock_guard<std::mutex> lk(m_);
    json out = json::array();
    for (json p : providers_) {
        const auto k = keys_.find(p.value("id", ""));
        p["has_key"] = k != keys_.end() && !k->second.empty();
        out.push_back(p);
    }
    return out;
}

bool AgentStore::provider(const std::string & id, json & out, std::string & key) const {
    std::lock_guard<std::mutex> lk(m_);
    out = find_by_id(providers_, id);
    if (out.is_null()) return false;
    const auto k = keys_.find(id);
    key = k == keys_.end() ? "" : k->second;
    out["has_key"] = !key.empty();
    return true;
}

bool AgentStore::put_provider(const json & provider, const std::string * key, std::string & err) {
    std::lock_guard<std::mutex> lk(m_);
    const std::string id = provider.value("id", "");
    json stored = provider;
    stored.erase("api_key");
    stored.erase("has_key");
    bool replaced = false;
    for (json & p : providers_)
        if (p.value("id", "") == id) {
            p = stored;
            replaced = true;
        }
    if (!replaced) providers_.push_back(stored);
    if (key) {
        if (key->empty())
            keys_.erase(id);
        else
            keys_[id] = *key;
        if (!save_credentials_locked(err)) return false;
    }
    save_list_locked("providers.json", providers_);
    return true;
}

bool AgentStore::remove_provider(const std::string & id) {
    std::lock_guard<std::mutex> lk(m_);
    if (!erase_by_id(providers_, id)) return false;
    keys_.erase(id);
    std::string err;
    if (!save_credentials_locked(err)) std::fprintf(stderr, "warning: %s\n", err.c_str());
    save_list_locked("providers.json", providers_);
    return true;
}

} // namespace bmoe::server::agent
