#include "settings_store.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace bmoe::server {

SettingsStore::SettingsStore(RunConfig defaults, std::string path)
    : defaults_(std::move(defaults)), path_(std::move(path)) {}

std::vector<std::string> SettingsStore::load() {
    std::vector<std::string> warnings;
    std::ifstream in(fs::u8path(path_));
    if (!in) return warnings; // first start
    json doc;
    try {
        in >> doc;
    } catch (const std::exception & e) {
        warnings.push_back("settings file " + path_ + " is not valid JSON (" + e.what() + "); starting from defaults");
        return warnings;
    }
    const json values = doc.value("values", json::object());
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = values.begin(); it != values.end(); ++it) {
        const ParamDesc * d = find_param(it.key());
        std::string s, err;
        RunConfig scratch;
        if (!d) {
            warnings.push_back("dropping saved setting '" + it.key() + "': this build has no such parameter");
        } else if (!value_to_param_string(it.value(), s) || !d->set(scratch, s, err)) {
            warnings.push_back("dropping saved setting '" + it.key() + "': " + (err.empty() ? "wrong type" : err));
        } else {
            user_[it.key()] = s;
        }
    }
    return warnings;
}

void SettingsStore::set_launch_values(std::map<std::string, std::string> values) {
    std::lock_guard<std::mutex> lk(m_);
    launch_ = std::move(values);
}

json SettingsStore::apply(const json & values, const std::vector<std::string> & reset) {
    json rejected = json::object();
    std::lock_guard<std::mutex> lk(m_);
    for (const std::string & k : reset) {
        user_.erase(k);
        launch_.erase(k);
    }
    for (auto it = values.begin(); it != values.end(); ++it) {
        const ParamDesc * d = find_param(it.key());
        if (!d) {
            rejected[it.key()] = "no such parameter";
            continue;
        }
        std::string s, err;
        RunConfig scratch;
        if (!value_to_param_string(it.value(), s)) {
            rejected[it.key()] = "expects a boolean, a number or a string";
            continue;
        }
        if (!d->set(scratch, s, err)) {
            rejected[it.key()] = err;
            continue;
        }
        user_[it.key()] = s;
        launch_.erase(it.key());
    }
    save_locked();
    return rejected;
}

void SettingsStore::apply_plan(std::map<std::string, std::string> values, json decisions) {
    std::lock_guard<std::mutex> lk(m_);
    plan_ = std::move(values);
    decisions_ = std::move(decisions);
}

RunConfig SettingsStore::build_locked() const {
    RunConfig c = defaults_;
    std::string err;
    for (const auto * layer : {&plan_, &launch_, &user_})
        for (const auto & kv : *layer)
            if (const ParamDesc * d = find_param(kv.first)) d->set(c, kv.second, err);
    return c;
}

RunConfig SettingsStore::config() const {
    std::lock_guard<std::mutex> lk(m_);
    return build_locked();
}

std::vector<std::string> SettingsStore::operator_keys() const {
    std::lock_guard<std::mutex> lk(m_);
    std::set<std::string> keys;
    for (const auto & kv : launch_)
        keys.insert(kv.first);
    for (const auto & kv : user_)
        keys.insert(kv.first);
    return {keys.begin(), keys.end()};
}

json SettingsStore::plan_decisions() const {
    std::lock_guard<std::mutex> lk(m_);
    return decisions_;
}

void SettingsStore::save_locked() const {
    json values = json::object();
    for (const auto & kv : user_)
        if (const ParamDesc * d = find_param(kv.first)) values[kv.first] = param_string_to_value(*d, kv.second);
    const json doc = {{"version", 1}, {"values", values}};

    // Write-then-rename, so a crash mid-write leaves the previous file rather than half of one.
    std::error_code ec;
    const fs::path p = fs::u8path(path_);
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << doc.dump(2) << "\n";
    }
    fs::rename(tmp, p, ec);
}

} // namespace bmoe::server
