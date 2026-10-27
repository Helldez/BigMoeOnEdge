// The server's side of the desktop app's packs and updates (docs/desktop-app.md): the signed feed
// read when a person asks, backend packs downloaded, checked and unpacked into <data>/backends/,
// and the installer of a newer release downloaded and checked for the app to run. Nothing here
// reaches the network unless one of its calls was made: starting the server, loading a model and
// chatting never do.
//
// HTTP stays in http_server.cpp: each call answers with a status and a body once it is done, and
// progress goes out meanwhile as `backend` events (a pack) and `update` events (the installer).
#pragma once

#include "backend_packs.h"
#include "event_bus.h"
#include "feed.h"
#include "http_fetch.h"
#include "json_util.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bmoe::server {

struct UpdateConfig {
    std::string data_dir;
    std::string feed_url;                // kFeedUrl unless --feed-url
    std::string public_key;              // base64; empty: this build reads no feed
    std::string target;                  // the build's own (platform::build_target)
    std::string version;                 // the running engine's
    std::vector<std::string> registered; // the backends that registered at start
    LibraryProbe library_loads;
    // Runs a command (argv[0] searched on PATH) and gives its exit status and output: how the
    // archive is listed and unpacked with the system's tar. A test hands a fake.
    std::function<int(const std::vector<std::string> & argv, std::string & output)> run;
};

class UpdateService {
public:
    struct Reply {
        int status = 200;
        json body;
    };

    UpdateService(EventBus & bus, HttpFetch & net, UpdateConfig cfg);
    ~UpdateService();

    // GET /api/backends. `refresh` reads the feed from the network; otherwise the last verified
    // feed kept on disk answers, or none.
    Reply backends(bool refresh);
    // POST /api/backends/<id>/install, answered once the pack is in place. `force` installs a pack
    // that does not apply here (a driver the probe could not load, a machine set up for later).
    Reply install_pack(const std::string & id, bool force);
    // GET /api/update: reads the feed (a person asked) and compares.
    Reply update();
    // POST /api/update/download: the installer of the feed last read, for this target, answered
    // once it is there and checked, with its path.
    Reply download_update();
    // Stops a download in progress: the server is stopping.
    void cancel();

    // The folder packs are installed in, and the one updates are downloaded to.
    std::string backends_root() const;
    std::string updates_dir() const;

private:
    Reply no_key() const;
    bool fetch_feed(std::string & err, int & status);
    bool load_cached_feed();
    void publish(const std::string & kind,
                 const std::string & id,
                 const std::string & file,
                 uint64_t received,
                 uint64_t total,
                 const char * state,
                 const std::string & error,
                 const json & extra = json::object());
    // One download at a time; its error, if any, is what the call answers with.
    Reply run_job(const std::function<void()> & fn, const json & ok);
    void run_install(const FeedPack & pack, std::string feed_version);
    void run_update_download(const FeedAsset & asset, std::string version);
    // Download `asset` to `dest`, its size and SHA-256 checked; events under (kind, id).
    bool
    fetch_checked(const std::string & kind, const std::string & id, const FeedAsset & asset, const std::string & dest);

    EventBus & bus_;
    HttpFetch & net_;
    UpdateConfig cfg_;
    Ed25519PublicKey key_{};
    bool have_key_ = false;

    std::mutex m_;
    std::unique_ptr<Feed> feed_; // the last verified feed, under m_
    bool restart_required_ = false;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stop_{false};
    std::string last_error_; // the running job's, while busy_
};

// Moves each pack installed while its older copy was in use (<root>/.pending-<id>) into place.
// Called before the engine loads anything from <root>, so a pack replaced on Windows, where a
// loaded library's folder cannot be renamed, takes effect at the next start. Returns the reasons
// a pending pack could not be moved, if any.
std::vector<std::string> finish_pending_packs(const std::string & backends_root);

// The system command runner UpdateConfig::run defaults to.
int run_system_command(const std::vector<std::string> & argv, std::string & output);

} // namespace bmoe::server
