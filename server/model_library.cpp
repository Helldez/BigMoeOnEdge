#include "model_library.h"

#include "bmoe/recipe.h"

#include "gguf.h"
#include "sheredom/subprocess.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
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

// "<stem>-00002-of-00005.gguf" -> stem, index 2, count 5. The naming llama.cpp's gguf-split writes
// and the engine resolves from the first shard.
const std::regex kShard(R"(^(.*)-(\d{5})-of-(\d{5})\.gguf$)", std::regex::icase);

} // namespace

ModelLibrary::ModelLibrary(std::string models_dir, std::string catalog_path, EventBus & bus)
    : dir_(std::move(models_dir)), bus_(bus) {
    std::error_code ec;
    fs::create_directories(fs::u8path(dir_), ec);

    std::ifstream in(fs::u8path(catalog_path));
    if (!in) {
        warnings_.push_back("no model catalog at " + catalog_path + ": the Models page lists local files only");
        return;
    }
    try {
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
            if (!e.files.empty()) catalog_.push_back(std::move(e));
        }
    } catch (const std::exception & ex) {
        warnings_.push_back("model catalog " + catalog_path + " is malformed: " + ex.what());
        catalog_.clear();
    }
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

std::string ModelLibrary::probe_arch(const std::string & path) {
    const fs::path p = fs::u8path(path);
    std::error_code ec;
    const uint64_t size = file_size_or_zero(p);
    const auto mtime = fs::last_write_time(p, ec);
    const long long mt = ec ? 0 : (long long) mtime.time_since_epoch().count();
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = arch_cache_.find(path);
        if (it != arch_cache_.end() && it->second.size == size && it->second.mtime == mt) return it->second.arch;
    }
    std::string arch;
    gguf_init_params gp = {/*no_alloc*/ true, /*ctx*/ nullptr};
    if (gguf_context * g = gguf_init_from_file(p.string().c_str(), gp)) {
        const int64_t k = gguf_find_key(g, "general.architecture");
        if (k >= 0 && gguf_get_kv_type(g, k) == GGUF_TYPE_STRING) arch = gguf_get_val_str(g, k);
        gguf_free(g);
    }
    std::lock_guard<std::mutex> lk(m_);
    arch_cache_[path] = {size, mt, arch};
    return arch;
}

const CatalogEntry * ModelLibrary::find(const std::string & id) const {
    for (const CatalogEntry & e : catalog_)
        if (e.id == id) return &e;
    return nullptr;
}

bool ModelLibrary::entry_on_disk(const CatalogEntry & e) const {
    for (const CatalogFile & f : e.files)
        if (!has_exact_size(fs::u8path(dir_) / fs::u8path(f.name), f.bytes)) return false;
    return true;
}

json ModelLibrary::list() {
    // Group the directory's ggufs: a split set is one model, listed by its first shard.
    struct Group {
        std::string first;
        int count = 1;
        std::set<int> present;
        uint64_t bytes = 0;
    };
    std::map<std::string, Group> groups;
    std::error_code ec;
    for (const auto & ent : fs::directory_iterator(fs::u8path(dir_), ec)) {
        if (!ent.is_regular_file(ec)) continue;
        const std::string name = ent.path().filename().u8string();
        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".gguf") != 0) continue;
        std::smatch sm;
        if (std::regex_match(name, sm, kShard)) {
            Group & g = groups[sm[1].str()];
            g.count = std::stoi(sm[3].str());
            g.present.insert(std::stoi(sm[2].str()));
            g.bytes += file_size_or_zero(ent.path());
            char first[64];
            std::snprintf(first, sizeof(first), "-00001-of-%05d.gguf", g.count);
            g.first = sm[1].str() + first;
        } else {
            Group & g = groups[name];
            g.first = name;
            g.present.insert(1);
            g.bytes = file_size_or_zero(ent.path());
        }
    }

    json local = json::array();
    for (auto & kv : groups) {
        const Group & g = kv.second;
        const fs::path first = fs::u8path(dir_) / fs::u8path(g.first);
        const bool complete = (int) g.present.size() == g.count;
        const std::string arch = g.present.count(1) ? probe_arch(first.u8string()) : "";
        local.push_back({{"name", g.first},
                         {"path", first.generic_u8string()},
                         {"bytes", g.bytes},
                         {"shards", g.count},
                         {"arch", arch},
                         {"streamable", !arch.empty() && find_moe_recipe(arch.c_str()) != nullptr},
                         {"complete", complete}});
    }

    json catalog = json::array();
    std::lock_guard<std::mutex> lk(m_);
    for (const CatalogEntry & e : catalog_) {
        uint64_t bytes = 0;
        for (const CatalogFile & f : e.files)
            bytes += f.bytes;
        auto job = jobs_.find(e.id);
        const bool running = job != jobs_.end() && !job->second->finished;
        const char * status = running ? "downloading" : entry_on_disk(e) ? "on_disk" : "available";
        catalog.push_back({{"id", e.id},
                           {"title", e.title},
                           {"quant", e.quant},
                           {"file", e.files.front().name},
                           {"bytes", bytes},
                           {"blurb", e.blurb},
                           {"status", status},
                           {"note", e.files.size() > 1 ? std::to_string(e.files.size()) + " shards" : ""}});
    }
    return json{{"models_dir", fs::u8path(dir_).generic_u8string()}, {"local", local}, {"catalog", catalog}};
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
    const CatalogEntry * e = find(id);
    if (!e) return "no catalog entry '" + id + "'";
    std::lock_guard<std::mutex> lk(m_);
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
