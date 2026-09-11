// The server's configuration, in layers.
//
//   server defaults  <  plan  <  launch flags  <  user
//
// Each layer holds parameter values as the strings the parameter table reads, keyed by parameter
// key, and the config is rebuilt by applying them in that order onto the defaults. Keeping the
// layers apart instead of one mutable RunConfig is what lets the UI say where a value came from,
// lets an improved default reach a user who never touched that knob, and gives the planner its
// pinned set: the launch and user layers are the operator's, and a plan never overrides them.
//
// Only the user layer is persisted (settings.json in the data directory). Launch flags hold for
// the process that was given them; setting the same key from the UI moves it to the user layer.
#pragma once

#include "json_util.h"

#include "bmoe/config.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bmoe::server {

class SettingsStore {
public:
    SettingsStore(RunConfig defaults, std::string path);

    // Read settings.json. Keys this build does not know, and values that no longer parse, are
    // dropped and described in the returned warnings rather than failing the start.
    std::vector<std::string> load();

    void set_launch_values(std::map<std::string, std::string> values);

    // Set `values` in the user layer and remove `reset` from every operator layer. Returns
    // {key: reason} for values that did not parse; those are left out. Persists.
    json apply(const json & values, const std::vector<std::string> & reset);

    // Replace the plan layer and remember the decisions behind it.
    void apply_plan(std::map<std::string, std::string> values, json decisions);

    RunConfig config() const;
    const RunConfig & defaults() const { return defaults_; }
    std::vector<std::string> operator_keys() const; // launch + user: what a plan must not touch
    json plan_decisions() const;

    // Whether every load plans first. A server setting, not an engine parameter, so it lives beside
    // the values rather than in the table. `fallback` is the answer until someone sets it.
    bool auto_plan(bool fallback) const;
    void set_auto_plan(bool on); // persists

private:
    RunConfig build_locked() const;
    void save_locked() const;

    mutable std::mutex m_;
    const RunConfig defaults_;
    const std::string path_;
    std::map<std::string, std::string> plan_, launch_, user_;
    json decisions_ = nullptr;
    bool auto_plan_set_ = false;
    bool auto_plan_ = false;
};

} // namespace bmoe::server
