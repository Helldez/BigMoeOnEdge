#include "model_library.h"

#include "model_files.h"

#include "bmoe/probe.h"

// The server never starts a child in a directory of its own, and the header's detection of the
// chdir file action trusts any libc that is not glibc or Apple's to have it, which bionic does not
// (its _np spelling arrives with API 34). Off everywhere, so the one header compiles on every libc.
#define SUBPROCESS_HAVE_CWD 0
#include "sheredom/subprocess.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace bmoe::server {

namespace {

uint64_t file_size_or_zero(const fs::path & p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : (uint64_t) n;
}

bool has_exact_size(const fs::path & p, uint64_t bytes) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && (bytes == 0 || file_size_or_zero(p) == bytes);
}

} // namespace

namespace {

// One catalog document's entries, as catalog/models.json spells them; the added file uses the
// same shape, so one reader serves both.
std::vector<CatalogEntry> read_catalog(std::istream & in) {
    std::vector<CatalogEntry> out;
    json doc;
    in >> doc;
    for (const json & m : doc.at("models")) {
        CatalogEntry e;
        e.id = m.at("id").get<std::string>();
        e.title = m.value("title", e.id);
        e.quant = m.value("quant", "");
        e.blurb = m.value("blurb", "");
        for (const json & f : m.at("files"))
            e.files.push_back(
                {f.at("name").get<std::string>(), f.at("url").get<std::string>(), f.value("bytes", (uint64_t) 0)});
        if (!e.files.empty()) out.push_back(std::move(e));
    }
    return out;
}

json catalog_json(const std::vector<CatalogEntry> & entries) {
    json models = json::array();
    for (const CatalogEntry & e : entries) {
        json files = json::array();
        for (const CatalogFile & f : e.files)
            files.push_back({{"name", f.name}, {"url", f.url}, {"bytes", f.bytes}});
        models.push_back({{"id", e.id}, {"title", e.title}, {"quant", e.quant}, {"blurb", e.blurb}, {"files", files}});
    }
    return json{{"version", 1}, {"models", models}};
}

} // namespace

ModelLibrary::ModelLibrary(std::string models_dir,
                           std::vector<std::string> scan_dirs,
                           std::string catalog_path,
                           std::string added_path,
                           EventBus & bus)
    : dir_(std::move(models_dir)), added_path_(std::move(added_path)), bus_(bus) {
    // A scan directory that is the models directory, or repeats another, would list its models twice.
    std::set<fs::path> seen;
    const auto key = [](const std::string & d) {
        std::error_code ec;
        const fs::path p = fs::weakly_canonical(fs::u8path(d), ec);
        return ec ? fs::u8path(d) : p;
    };
    seen.insert(key(dir_));
    for (std::string & d : scan_dirs)
        if (!d.empty() && seen.insert(key(d)).second) scan_dirs_.push_back(std::move(d));

    std::error_code ec;
    fs::create_directories(fs::u8path(dir_), ec);

    std::ifstream in(fs::u8path(catalog_path));
    if (!in) {
        warnings_.push_back("no model catalog at " + catalog_path + ": the Models page lists local files only");
        return;
    }
    try {
        catalog_ = read_catalog(in);
    } catch (const std::exception & ex) {
        warnings_.push_back("model catalog " + catalog_path + " is malformed: " + ex.what());
        catalog_.clear();
    }
    if (added_path_.empty()) return;
    std::ifstream added(fs::u8path(added_path_));
    if (!added) return; // nothing added yet
    try {
        added_ = read_catalog(added);
    } catch (const std::exception & ex) {
        warnings_.push_back("the added models file " + added_path_ + " is malformed and is ignored: " + ex.what());
        added_.clear();
    }
}

void ModelLibrary::save_added() {
    if (added_path_.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::u8path(added_path_).parent_path(), ec);
    std::ofstream out(fs::u8path(added_path_), std::ios::trunc);
    out << catalog_json(added_).dump(2) << "\n";
}

std::string ModelLibrary::add_entry(const CatalogEntry & e) {
    if (e.id.empty() || e.files.empty()) return "an entry needs an id and at least one file";
    for (const CatalogFile & f : e.files)
        if (f.name.empty() || f.name.find_first_of("/\\") != std::string::npos || f.name == "." || f.name == "..")
            return "a file name must be a plain name";
    std::lock_guard<std::mutex> lk(m_);
    for (const CatalogEntry & c : catalog_)
        if (c.id == e.id) return "the catalog already has " + e.id;
    for (CatalogEntry & a : added_)
        if (a.id == e.id) {
            a = e;
            save_added();
            return "";
        }
    added_.push_back(e);
    save_added();
    return "";
}

bool ModelLibrary::remove_entry(const std::string & id) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = added_.begin(); it != added_.end(); ++it)
        if (it->id == id) {
            added_.erase(it);
            save_added();
            return true;
        }
    return false;
}

bool ModelLibrary::entry(const std::string & id, CatalogEntry & out) const {
    std::lock_guard<std::mutex> lk(m_);
    if (const CatalogEntry * e = find(id)) {
        out = *e;
        return true;
    }
    return false;
}

ModelLibrary::~ModelLibrary() {
    std::map<std::string, std::unique_ptr<Job>> jobs;
    {
        std::lock_guard<std::mutex> lk(m_);
        jobs.swap(jobs_);
    }
    for (auto & kv : jobs) {
        kv.second->cancel = true;
        {
            std::lock_guard<std::mutex> lk(kv.second->proc_m);
            if (kv.second->proc) subprocess_terminate(static_cast<subprocess_s *>(kv.second->proc));
        }
        if (kv.second->thread.joinable()) kv.second->thread.join();
    }
}

ModelLibrary::ModelFacts ModelLibrary::probe(const std::string & path) {
    const fs::path p = fs::u8path(path);
    std::error_code ec;
    const uint64_t size = file_size_or_zero(p);
    const auto mtime = fs::last_write_time(p, ec);
    const long long mt = ec ? 0 : (long long) mtime.time_since_epoch().count();
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = facts_cache_.find(path);
        if (it != facts_cache_.end() && it->second.size == size && it->second.mtime == mt) return it->second.facts;
    }
    // The planner's own reading of the file - the headers of every shard, no tensor bytes - so the
    // listing, the plan and the load agree on what a file is. Streamable means it declares routed
    // experts laid out the way the engine streams them; a dense model reads fine and is not.
    const ModelProfile m = probe_model(path.c_str());
    ModelFacts facts;
    facts.arch = m.arch;
    facts.streamable = m.ok && m.is_moe;
    std::lock_guard<std::mutex> lk(m_);
    facts_cache_[path] = {size, mt, facts};
    return facts;
}

const CatalogEntry * ModelLibrary::find(const std::string & id) const {
    for (const CatalogEntry & e : catalog_)
        if (e.id == id) return &e;
    for (const CatalogEntry & e : added_)
        if (e.id == id) return &e;
    return nullptr;
}

bool ModelLibrary::entry_on_disk(const CatalogEntry & e) const {
    for (const CatalogFile & f : e.files)
        if (!has_exact_size(fs::u8path(dir_) / fs::u8path(f.name), f.bytes)) return false;
    return true;
}

void ModelLibrary::list_dir(const std::string & dir, json & out) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto & ent : fs::directory_iterator(fs::u8path(dir), ec))
        if (ent.is_regular_file(ec)) names.push_back(ent.path().filename().u8string());

    const fs::path base = fs::u8path(dir);
    for (const ModelFiles & m : group_model_files(names)) {
        uint64_t bytes = 0;
        json files = json::array();
        for (const std::string & f : m.files) {
            bytes += file_size_or_zero(base / fs::u8path(f));
            files.push_back((base / fs::u8path(f)).generic_u8string());
        }
        const fs::path first = base / fs::u8path(m.first);
        // Only the first shard carries the metadata; a set without it has no architecture to read.
        const bool has_first = !m.files.empty() && m.files.front() == m.first;
        const ModelFacts facts = has_first ? probe(first.u8string()) : ModelFacts{};
        out.push_back({{"name", m.first},
                       {"path", first.generic_u8string()},
                       {"dir", base.generic_u8string()},
                       {"bytes", bytes},
                       {"shards", m.count},
                       {"files", files},
                       {"arch", facts.arch},
                       {"streamable", facts.streamable},
                       {"complete", m.complete()}});
    }
}

json ModelLibrary::list() {
    // The models directory first, then each scan directory in the order given: the order a host
    // app ranks them in (a real filesystem before an emulated one).
    json local = json::array();
    list_dir(dir_, local);
    for (const std::string & d : scan_dirs_)
        list_dir(d, local);

    json catalog = json::array();
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::pair<const CatalogEntry *, bool>> entries; // entry, added at run time
    for (const CatalogEntry & e : catalog_)
        entries.emplace_back(&e, false);
    for (const CatalogEntry & e : added_)
        entries.emplace_back(&e, true);
    for (const auto & [ep, added] : entries) {
        const CatalogEntry & e = *ep;
        uint64_t bytes = 0;
        for (const CatalogFile & f : e.files)
            bytes += f.bytes;
        auto job = jobs_.find(e.id);
        const bool running = job != jobs_.end() && !job->second->finished;
        const char * status = running ? "downloading" : entry_on_disk(e) ? "on_disk" : "available";
        // Every file with its URL: a host that has no curl (a phone) downloads for itself, and the
        // catalog is still the one place the URLs are written.
        json files = json::array();
        for (const CatalogFile & f : e.files)
            files.push_back({{"name", f.name}, {"bytes", f.bytes}, {"url", f.url}});
        catalog.push_back({{"id", e.id},
                           {"title", e.title},
                           {"quant", e.quant},
                           {"file", e.files.front().name},
                           {"bytes", bytes},
                           {"files", files},
                           {"blurb", e.blurb},
                           {"status", status},
                           {"added", added},
                           {"note", e.files.size() > 1 ? std::to_string(e.files.size()) + " shards" : ""}});
    }
    json scan = json::array();
    for (const std::string & d : scan_dirs_)
        scan.push_back(fs::u8path(d).generic_u8string());
    return json{{"models_dir", fs::u8path(dir_).generic_u8string()},
                {"scan_dirs", scan},
                {"local", local},
                {"catalog", catalog}};
}

void ModelLibrary::publish(const std::string & id,
                           const std::string & file,
                           uint64_t received,
                           uint64_t total,
                           const char * state,
                           const std::string & error) {
    bus_.publish(
        "download",
        json{{"id", id}, {"file", file}, {"received", received}, {"total", total}, {"state", state}, {"error", error}});
}

std::string ModelLibrary::start_download(const std::string & id) {
    std::lock_guard<std::mutex> lk(m_);
    const CatalogEntry * e = find(id);
    if (!e) return "no catalog entry '" + id + "'";
    auto it = jobs_.find(id);
    if (it != jobs_.end()) {
        if (!it->second->finished) return "";
        if (it->second->thread.joinable()) it->second->thread.join();
        jobs_.erase(it);
    }
    auto job = std::make_unique<Job>();
    Job * j = job.get();
    const CatalogEntry entry = *e;
    job->thread = std::thread([this, entry, j] {
        run_download(entry, *j);
        j->finished = true;
    });
    jobs_[id] = std::move(job);
    return "";
}

bool ModelLibrary::cancel_download(const std::string & id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second->finished) return false;
    it->second->cancel = true;
    std::lock_guard<std::mutex> plk(it->second->proc_m);
    if (it->second->proc) subprocess_terminate(static_cast<subprocess_s *>(it->second->proc));
    return true;
}

void ModelLibrary::run_download(const CatalogEntry & e, Job & job) {
    uint64_t total = 0;
    for (const CatalogFile & f : e.files)
        total += f.bytes;
    uint64_t done = 0;

    for (const CatalogFile & f : e.files) {
        const fs::path final_path = fs::u8path(dir_) / fs::u8path(f.name);
        const fs::path part = fs::u8path(final_path.u8string() + ".part");
        std::error_code ec;
        if (has_exact_size(final_path, f.bytes)) {
            done += f.bytes;
            continue;
        }
        // A part that already holds every byte only lacks its rename; curl would answer a resume
        // past the end with an error.
        if (f.bytes > 0 && file_size_or_zero(part) == f.bytes) {
            fs::rename(part, final_path, ec);
            done += f.bytes;
            continue;
        }

        const std::string part_s = part.string();
        const char * cmd[] = {"curl", "-L", "--fail", "--retry",      "3",           "-sS",
                              "-C",   "-",  "-o",     part_s.c_str(), f.url.c_str(), nullptr};
        subprocess_s proc;
        const int opts = subprocess_option_search_user_path | subprocess_option_inherit_environment |
                         subprocess_option_no_window | subprocess_option_combined_stdout_stderr;
        if (subprocess_create(cmd, opts, &proc) != 0) {
            publish(e.id, f.name, done, total, "error", "could not start curl: is it installed and on PATH?");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(job.proc_m);
            job.proc = &proc;
        }
        while (subprocess_alive(&proc)) {
            publish(e.id, f.name, done + file_size_or_zero(part), total, "running", "");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        int rc = 0;
        subprocess_join(&proc, &rc);
        std::string output;
        if (FILE * out = subprocess_stdout(&proc)) {
            char buf[256];
            while (std::fgets(buf, sizeof(buf), out))
                output += buf;
        }
        {
            std::lock_guard<std::mutex> lk(job.proc_m);
            job.proc = nullptr;
        }
        subprocess_destroy(&proc);

        if (job.cancel) {
            publish(e.id, f.name, done + file_size_or_zero(part), total, "cancelled", "");
            return;
        }
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
            output.pop_back();
        if (rc != 0) {
            publish(e.id, f.name, done + file_size_or_zero(part), total, "error",
                    "curl exited with " + std::to_string(rc) + (output.empty() ? "" : ": " + output));
            return;
        }
        if (f.bytes > 0 && file_size_or_zero(part) != f.bytes) {
            publish(e.id, f.name, done + file_size_or_zero(part), total, "error",
                    "downloaded size does not match the catalog; the partial file is kept");
            return;
        }
        fs::rename(part, final_path, ec);
        if (ec) {
            publish(e.id, f.name, done, total, "error", "could not rename the finished file: " + ec.message());
            return;
        }
        done += f.bytes;
    }
    publish(e.id, e.files.back().name, total, total, "done", "");
}

} // namespace bmoe::server
