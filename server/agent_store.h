// What the agent keeps on disk (docs/agent.md): conversations, workspaces, which skills are switched
// on, tasks, providers, and the providers' keys. Each lives in a JSON file under `<data>/agent/`,
// written whole to a temporary file and renamed over the old one, so a crash mid-write leaves the
// previous version and never half of one. The keys are the exception to everything being readable:
// `<data>/credentials.json`, created readable by this user only, never returned by any listing,
// never logged.
//
// Thread-safe: one mutex over every collection. The run loop, the scheduler and the HTTP handlers
// all write here; nothing holds the lock while it waits for a model or a person.
#pragma once

#include "json_util.h"

#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bmoe::server::agent {

// A new identifier: a time prefix that sorts by creation, then random hex. `prefix` names the kind
// ("c" conversation, "i" item, ...), so an id read in a log says what it is.
std::string new_id(const char * prefix);

// Write `text` to `path` through a temporary file and a rename. `owner_only`: the file is created
// readable and writable by this user alone before any byte is written. False with the reason.
bool write_file_atomic(const std::string & path, const std::string & text, bool owner_only, std::string & err);

std::time_t now_s();

// A conversation reaches this computer when it has a workspace, sends to a saved provider (whose key
// the server holds) or is another agent's (a program here): it, its events and its changes answer to
// a local client only.
bool conversation_reaches(const json & conv);

// Whether a create or change of a conversation sets what a client on the network may not: a
// workspace, a provider route, another agent, or full access (which would let a later local run act without
// asking). Asking and reading only are the safe direction, so the web UI on a phone still sets them.
bool network_may_not_set(const json & fields);

class AgentStore {
public:
    // `data_dir` is the server's data directory; the store keeps its files below it.
    explicit AgentStore(std::string data_dir);

    // Read everything from disk. A file that does not parse is set aside (renamed `.bad`) and named
    // in the warnings, never a failed start. A conversation that was running when the server stopped
    // reads `idle`, with a notice saying its run ended there.
    std::vector<std::string> load();

    // ---- conversations ----
    // {id, title, workspace, mode, route, agent, acp_session?, state, created, updated, task, items}
    // Summaries without items, newest first.
    json list_conversations() const;
    bool conversation(const std::string & id, json & out) const;
    // conversation_reaches() of a conversation, read in place (no copy: asked on every streamed
    // token). False when there is no such conversation.
    bool reaches(const std::string & id) const;
    json create_conversation(json fields);
    // Apply `edit` to the conversation under the lock, stamp `updated`, persist when `persist`.
    // False when there is no such conversation. `after`, when given, receives the result.
    bool edit_conversation(const std::string & id,
                           const std::function<void(json &)> & edit,
                           bool persist = true,
                           json * after = nullptr);
    bool remove_conversation(const std::string & id);
    // Add the item, or replace the one with its id. Persists when `persist` (a streaming assistant
    // item is persisted when it ends, not on every token).
    bool upsert_item(const std::string & conversation, const json & item, bool persist = true);
    bool remove_item(const std::string & conversation, const std::string & item);

    // ---- workspaces: [{id, name, path}] ----
    json workspaces() const;
    bool workspace(const std::string & id, json & out) const;
    // The folder must exist; it is kept canonical. The same folder added twice is the same workspace.
    bool add_workspace(const std::string & path, const std::string & name, json & out, std::string & err);
    bool remove_workspace(const std::string & id);

    // ---- skills: which are switched off or on, by name; one never set is on ----
    std::map<std::string, bool> skill_switches() const;
    void set_skill_enabled(const std::string & name, bool enabled);

    // ---- tasks ----
    json tasks() const;
    bool task(const std::string & id, json & out) const;
    void put_task(const json & task); // by its id
    bool remove_task(const std::string & id);

    // ---- providers: [{id, name, base_url, models, has_key}] ----
    json providers() const;
    // The provider and its key ("" when none). The only way to read a key, and only the run loop does.
    bool provider(const std::string & id, json & out, std::string & key) const;
    // `key`: nullptr keeps the stored one, "" removes it, anything else replaces it.
    bool put_provider(const json & provider, const std::string * key, std::string & err);
    bool remove_provider(const std::string & id);

    const std::string & data_dir() const { return data_dir_; }

private:
    void save_conversation_locked(const json & c) const;
    void save_list_locked(const std::string & file, const json & j) const;
    bool save_credentials_locked(std::string & err) const;
    std::string dir() const;

    const std::string data_dir_;
    mutable std::mutex m_;
    std::map<std::string, json> conversations_;
    json workspaces_ = json::array();
    json tasks_ = json::array();
    json providers_ = json::array();
    std::map<std::string, bool> skills_;
    std::map<std::string, std::string> keys_; // provider id -> key
};

} // namespace bmoe::server::agent
