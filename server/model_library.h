// The models on disk, the curated catalog, and downloads between the two.
//
// The catalog is data (catalog/models.json), not code: which models are offered changes far more
// often than how they are fetched. Downloads run the system `curl` (present on Windows 10+, macOS
// and every Linux distribution this targets) rather than linking a TLS stack into the server: it
// resumes with -C -, follows the Hugging Face redirects, and keeps certificates the OS's business.
#pragma once

#include "event_bus.h"
#include "json_util.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bmoe::server {

struct CatalogFile {
    std::string name;
    std::string url;
    uint64_t bytes = 0;
};

struct CatalogEntry {
    std::string id, title, quant, blurb;
    std::vector<CatalogFile> files;
};

class ModelLibrary {
public:
    ModelLibrary(std::string models_dir, std::string catalog_path, EventBus & bus);
    ~ModelLibrary();

    // Problems found reading the catalog, for the start-up log. Empty when it loaded cleanly.
    const std::vector<std::string> & warnings() const { return warnings_; }
    const std::string & models_dir() const { return dir_; }

    // The /api/models object: local files and the catalog with each entry's status.
    json list();

    // general.architecture of a gguf, read from its metadata only; "" when unreadable.
    std::string probe_arch(const std::string & path);

    std::string start_download(const std::string & id); // "" once started
    bool cancel_download(const std::string & id);

private:
    struct Job {
        std::thread thread;
        std::atomic<bool> cancel{false};
        std::atomic<bool> finished{false};
        std::mutex proc_m;
        void * proc = nullptr; // the running curl (subprocess_s*), for cancel
    };

    void run_download(const CatalogEntry & e, Job & job);
    void publish(const std::string & id,
                 const std::string & file,
                 uint64_t received,
                 uint64_t total,
                 const char * state,
                 const std::string & error);
    const CatalogEntry * find(const std::string & id) const;
    bool entry_on_disk(const CatalogEntry & e) const;

    std::string dir_;
    EventBus & bus_;
    std::vector<CatalogEntry> catalog_;
    std::vector<std::string> warnings_;

    std::mutex m_;
    std::map<std::string, std::unique_ptr<Job>> jobs_;
    struct ArchCacheEntry {
        uint64_t size = 0;
        long long mtime = 0;
        std::string arch;
    };
    std::map<std::string, ArchCacheEntry> arch_cache_;
};

} // namespace bmoe::server
