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
#include <chrono>
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
    // `models_dir` is where downloads land. `scan_dirs` are other directories whose models are
    // listed beside it and loaded in place, never written (a phone's adb-pushed and shared-storage
    // folders): the host app knows where they are, the server says what they hold.
    // `added_path` is where entries added at run time (a model picked from a repository,
    // hf_repo.h) are kept between runs, beside the settings; "" keeps none.
    ModelLibrary(std::string models_dir,
                 std::vector<std::string> scan_dirs,
                 std::string catalog_path,
                 std::string added_path,
                 EventBus & bus);
    ~ModelLibrary();

    // Problems found reading the catalog, for the start-up log. Empty when it loaded cleanly.
    const std::vector<std::string> & warnings() const { return warnings_; }
    const std::string & models_dir() const { return dir_; }
    // Every folder this library lists models from: the models folder first, then the scan dirs.
    std::vector<std::string> model_roots() const {
        std::vector<std::string> r{dir_};
        r.insert(r.end(), scan_dirs_.begin(), scan_dirs_.end());
        return r;
    }

    // The /api/models object: local files and the catalog with each entry's status.
    json list();

    // What a gguf is, from its headers alone (no tensor bytes): its architecture, and whether it
    // carries routed experts in a layout this engine streams (docs/which-models-stream.md). "" and
    // false when unreadable.
    struct ModelFacts {
        std::string arch;
        bool streamable = false;
    };
    ModelFacts probe(const std::string & path);

    // Add (or replace, by id) an entry the server built, and keep it. "" when kept; otherwise
    // why it was not.
    std::string add_entry(const CatalogEntry & e);
    // Forget an added entry. Files on disk stay. False when there is no such added entry.
    bool remove_entry(const std::string & id);
    // The entry, when the catalog or the added ones have it.
    bool entry(const std::string & id, CatalogEntry & out) const;

    // Why a download did not start, as the HTTP status that says so; status 0 once started (or
    // already running).
    struct DownloadRefusal {
        int status = 0; // 404: no such entry; 507: the disk cannot take what is left to download
        std::string error;
    };
    DownloadRefusal start_download(const std::string & id);
    // Stop a running download. Its partial files stay, so the next start resumes them.
    bool cancel_download(const std::string & id);
    // Delete the partial files (`.part`) of entry `id` in the models folder: the one way to give a
    // stopped download's bytes back. Only names the entry lists, only in the models folder (never a
    // scan directory), and never while it downloads. status 0 when done, with the bytes freed.
    DownloadRefusal delete_partial(const std::string & id, uint64_t & freed);

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
    // What entry `e` already has in the models folder: complete files plus the bytes of each
    // `.part`, and whether any `.part` is there - what a resume would continue from.
    struct OnDisk {
        uint64_t received = 0;
        uint64_t still_needed = 0;
        bool partial = false;
    };
    OnDisk on_disk(const CatalogEntry & e) const;
    // The job of `id` runs: its entry's files are being written. Called with m_ held.
    bool running_locked(const std::string & id) const;
    // Wait `d`, waking early when the job is cancelled. False when it was.
    bool wait_unless_cancelled(Job & job, std::chrono::seconds d);

    // One directory's models as /api/models `local` entries, appended to `out`.
    void list_dir(const std::string & dir, json & out);

    void save_added();
    std::string dir_;
    std::vector<std::string> scan_dirs_;
    std::string added_path_;
    std::vector<CatalogEntry> added_;
    EventBus & bus_;
    std::vector<CatalogEntry> catalog_;
    std::vector<std::string> warnings_;

    mutable std::mutex m_;
    std::map<std::string, std::unique_ptr<Job>> jobs_;
    struct FactsCacheEntry {
        uint64_t size = 0;
        long long mtime = 0;
        ModelFacts facts;
    };
    std::map<std::string, FactsCacheEntry> facts_cache_;
};

} // namespace bmoe::server
