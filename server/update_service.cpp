#include "update_service.h"

#include "download_policy.h"
#include "sha256.h"

// See model_library.cpp for why the subprocess header's chdir support is off.
#define SUBPROCESS_HAVE_CWD 0
#include "sheredom/subprocess.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace bmoe::server {

namespace {

constexpr const char * kFeedCacheDir = "feed";
constexpr const char * kFeedFile = "feed.json";
constexpr const char * kSignatureSuffix = ".sig";

bool read_file(const fs::path & p, std::string & out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const fs::path & p, const std::string & data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << data;
    return (bool) out;
}

std::vector<std::string> lines_of(const std::string & text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

} // namespace

int run_system_command(const std::vector<std::string> & argv, std::string & output) {
    output.clear();
    std::vector<const char *> cmd;
    for (const std::string & a : argv)
        cmd.push_back(a.c_str());
    cmd.push_back(nullptr);
    subprocess_s proc;
    const int opts = subprocess_option_search_user_path | subprocess_option_inherit_environment |
                     subprocess_option_no_window | subprocess_option_combined_stdout_stderr;
    if (subprocess_create(cmd.data(), opts, &proc) != 0) return -1;
    // Read before the join: a listing can be longer than a pipe holds.
    if (FILE * o = subprocess_stdout(&proc)) {
        char buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), o)) > 0)
            output.append(buf, n);
    }
    int rc = 0;
    subprocess_join(&proc, &rc);
    subprocess_destroy(&proc);
    return rc;
}

std::vector<std::string> finish_pending_packs(const std::string & backends_root) {
    std::vector<std::string> problems;
    std::error_code ec;
    const fs::path root = fs::u8path(backends_root);
    static const std::string prefix = ".pending-";
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string id = name.substr(prefix.size());
        if (!is_pack_id(id)) continue;
        const fs::path dest = root / fs::u8path(id);
        std::error_code rec;
        fs::remove_all(dest, rec);
        fs::rename(it->path(), dest, rec);
        if (rec) problems.push_back("could not move the new " + id + " pack into place: " + rec.message());
    }
    // Copies an earlier swap moved aside and could not delete then.
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename().u8string().compare(0, 5, ".old-") == 0) {
            std::error_code rec;
            fs::remove_all(it->path(), rec);
        }
    return problems;
}

UpdateService::UpdateService(EventBus & bus, HttpFetch & net, UpdateConfig cfg)
    : bus_(bus), net_(net), cfg_(std::move(cfg)) {
    have_key_ = !cfg_.public_key.empty() && parse_public_key(cfg_.public_key, key_);
    if (!cfg_.run) cfg_.run = run_system_command;
}

UpdateService::~UpdateService() {
    stop_ = true;
}

std::string UpdateService::backends_root() const {
    return (fs::u8path(cfg_.data_dir) / "backends").u8string();
}

std::string UpdateService::updates_dir() const {
    return (fs::u8path(cfg_.data_dir) / "updates").u8string();
}

UpdateService::Reply UpdateService::no_key() const {
    return {503, json{{"error", "this build carries no key to check the release feed with, so it reads none"},
                      {"code", "no_feed_key"}}};
}

void UpdateService::publish(const std::string & kind,
                            const std::string & id,
                            const std::string & file,
                            uint64_t received,
                            uint64_t total,
                            const char * state,
                            const std::string & error,
                            const json & extra) {
    json e{{"id", id},       {"kind", kind},   {"file", file},  {"received", received},
           {"total", total}, {"state", state}, {"error", error}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        e[it.key()] = it.value();
    if (std::string(state) != "running" && std::string(state) != "done") last_error_ = error;
    // An event of its own per kind: "backend" for a pack, "update" for the installer.
    bus_.publish(kind, e);
}

bool UpdateService::fetch_feed(std::string & err, int & status) {
    std::string body, sig;
    // The signature first: it is the small one, and without it the feed is not read at all.
    if (!net_.get(cfg_.feed_url + kSignatureSuffix, 0, 0, "", sig, err)) {
        err = "could not read the feed's signature: " + err;
        status = 502;
        return false;
    }
    if (!net_.get(cfg_.feed_url, 0, 0, "", body, err)) {
        err = "could not read the feed: " + err;
        status = 502;
        return false;
    }
    auto feed = std::make_unique<Feed>();
    if (!read_signed_feed(body, sig, key_, *feed, err)) {
        status = 502;
        return false;
    }
    // Kept as it came, signature and all, and checked again whenever it is read back.
    std::error_code ec;
    const fs::path dir = fs::u8path(cfg_.data_dir) / kFeedCacheDir;
    fs::create_directories(dir, ec);
    write_file(dir / kFeedFile, body);
    write_file(dir / (std::string(kFeedFile) + kSignatureSuffix), sig);
    std::lock_guard<std::mutex> lk(m_);
    feed_ = std::move(feed);
    return true;
}

bool UpdateService::load_cached_feed() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (feed_) return true;
    }
    const fs::path dir = fs::u8path(cfg_.data_dir) / kFeedCacheDir;
    std::string body, sig, err;
    if (!read_file(dir / kFeedFile, body) || !read_file(dir / (std::string(kFeedFile) + kSignatureSuffix), sig))
        return false;
    auto feed = std::make_unique<Feed>();
    if (!read_signed_feed(body, sig, key_, *feed, err)) return false;
    std::lock_guard<std::mutex> lk(m_);
    feed_ = std::move(feed);
    return true;
}

UpdateService::Reply UpdateService::backends(bool refresh) {
    std::string err;
    int status = 200;
    // A feed that could not be read is said beside what is known without it (the backends that
    // registered, the packs installed, the feed kept from before), not instead of it.
    std::string feed_error;
    if (have_key_) {
        if (refresh && !fetch_feed(err, status)) feed_error = err;
        load_cached_feed();
    } else if (refresh) {
        feed_error = no_key().body["error"].get<std::string>();
    }
    std::lock_guard<std::mutex> lk(m_);
    json body = backends_answer(cfg_.registered, cfg_.target, feed_.get(), installed_packs(backends_root()),
                                cfg_.library_loads, restart_required_);
    if (!feed_error.empty()) body["feed_error"] = feed_error;
    return {200, body};
}

UpdateService::Reply UpdateService::update() {
    if (!have_key_) return no_key();
    std::string err;
    int status = 200;
    if (!fetch_feed(err, status)) return {status, json{{"error", err}, {"code", "feed"}}};
    std::lock_guard<std::mutex> lk(m_);
    return {200, update_answer(*feed_, cfg_.version, cfg_.target)};
}

void UpdateService::cancel() {
    stop_ = true;
}

UpdateService::Reply UpdateService::run_job(const std::function<void()> & fn, const json & ok) {
    if (busy_.exchange(true)) return {409, json{{"error", "a download is already running"}, {"code", "busy"}}};
    last_error_.clear();
    fn();
    const std::string err = last_error_;
    busy_ = false;
    if (!err.empty()) return {stop_ ? 503 : 502, json{{"error", err}, {"code", stop_ ? "cancelled" : "failed"}}};
    return {200, ok};
}

UpdateService::Reply UpdateService::install_pack(const std::string & id, bool force) {
    if (!have_key_) return no_key();
    if (!is_pack_id(id)) return {400, json{{"error", "not a pack id"}, {"code", "invalid_request"}}};
    load_cached_feed();
    FeedPack pack;
    std::string version;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!feed_)
            return {409, json{{"error", "no feed has been read yet: ask GET /api/backends?refresh=1 first"},
                              {"code", "no_feed"}}};
        bool found = false;
        for (const FeedPack & p : feed_->packs)
            if (p.id == id && p.target == cfg_.target) {
                pack = p;
                found = true;
            }
        if (!found)
            return {404,
                    json{{"error", "the feed offers no pack '" + id + "' for " + cfg_.target}, {"code", "not_found"}}};
        version = feed_->version;
    }
    if (!force && !pack_applies(pack, cfg_.library_loads))
        return {409, json{{"error", "none of the libraries this pack needs loads on this machine"},
                          {"code", "not_applicable"}}};
    // Answered once the pack is in place (or refused): progress meanwhile as `backend` events.
    return run_job([this, pack, version] { run_install(pack, version); },
                   json{{"ok", true}, {"id", id}, {"restart_required", true}});
}

UpdateService::Reply UpdateService::download_update() {
    if (!have_key_) return no_key();
    FeedAsset asset;
    std::string version;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!feed_)
            return {409, json{{"error", "no feed has been read yet: ask GET /api/update first"}, {"code", "no_feed"}}};
        const json answer = update_answer(*feed_, cfg_.version, cfg_.target);
        if (!answer["available"].get<bool>())
            return {409, json{{"error", "no newer release for " + cfg_.target}, {"code", "up_to_date"}}};
        asset = feed_->installers.at(cfg_.target);
        version = feed_->version;
    }
    const std::string path = (fs::u8path(updates_dir()) / fs::u8path(asset.file_name())).u8string();
    // Answered once the installer is there and checked, with the path the app runs it from.
    return run_job([this, asset, version] { run_update_download(asset, version); },
                   json{{"ok", true}, {"version", version}, {"path", path}});
}

bool UpdateService::fetch_checked(const std::string & kind,
                                  const std::string & id,
                                  const FeedAsset & asset,
                                  const std::string & dest) {
    const std::string file = asset.file_name();
    std::error_code ec;
    const uint64_t free_bytes = [&] {
        const fs::space_info s = fs::space(fs::u8path(dest).parent_path(), ec);
        return ec ? 0 : (uint64_t) s.available;
    }();
    const std::string room = free_space_refusal(asset.size, free_bytes);
    if (!room.empty()) {
        publish(kind, id, file, 0, asset.size, "error", room);
        return false;
    }
    std::string err;
    publish(kind, id, file, 0, asset.size, "running", "");
    const bool got = net_.download(
        asset.url, dest,
        [&](uint64_t received) {
            publish(kind, id, file, received, asset.size, "running", "");
            return !stop_.load();
        },
        err);
    if (!got) {
        publish(kind, id, file, 0, asset.size, stop_ ? "cancelled" : "error", err);
        return false;
    }
    const uint64_t size = (uint64_t) fs::file_size(fs::u8path(dest), ec);
    if (ec || size != asset.size) {
        fs::remove(fs::u8path(dest), ec);
        publish(kind, id, file, size, asset.size, "error", "the file's size is not the one the feed names");
        return false;
    }
    publish(kind, id, file, size, asset.size, "running", "checking the file's SHA-256");
    std::string actual;
    if (!sha256_file(dest, actual, err, [&](uint64_t) { return !stop_.load(); })) {
        fs::remove(fs::u8path(dest), ec);
        publish(kind, id, file, size, asset.size, stop_ ? "cancelled" : "error", err);
        return false;
    }
    const std::string refusal = digest_refusal(asset.sha256, actual);
    if (!refusal.empty()) {
        fs::remove(fs::u8path(dest), ec);
        publish(kind, id, file, size, asset.size, "error", refusal + "; the download is deleted");
        return false;
    }
    return true;
}

void UpdateService::run_install(const FeedPack & pack, std::string feed_version) {
    const std::string kind = "backend";
    const std::string file = pack.asset.file_name();
    std::error_code ec;
    const fs::path root = fs::u8path(backends_root());
    const fs::path scratch = fs::u8path(cfg_.data_dir) / "tmp";
    fs::create_directories(scratch, ec);
    fs::create_directories(root, ec);
    const fs::path archive = scratch / fs::u8path("pack-" + pack.id + "-" + file);
    const std::string archive_s = archive.u8string();
    if (!fetch_checked(kind, pack.id, pack.asset, archive_s)) return;
    auto fail = [&](const std::string & why) {
        fs::remove(archive, ec);
        publish(kind, pack.id, file, pack.asset.size, pack.asset.size, "error", why);
    };

    // What the archive holds, as tar itself reads it, checked before a byte is unpacked.
    publish(kind, pack.id, file, pack.asset.size, pack.asset.size, "running", "unpacking");
    std::string names_out, verbose_out;
    if (cfg_.run({"tar", "-tf", archive.string()}, names_out) != 0 ||
        cfg_.run({"tar", "-tvf", archive.string()}, verbose_out) != 0)
        return fail("tar could not read the archive: is tar installed and on PATH?");
    const std::string listing = archive_listing_refusal(lines_of(names_out), lines_of(verbose_out));
    if (!listing.empty()) return fail("the archive is refused: " + listing);

    // Unpacked into a folder of its own, checked again as files, and only then put in place.
    const fs::path staging = root / fs::u8path(".staging-" + pack.id);
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    std::string out;
    if (cfg_.run({"tar", "-xf", archive.string(), "-C", staging.string()}, out) != 0) {
        fs::remove_all(staging, ec);
        return fail("tar could not unpack the archive: " + out);
    }
    fs::remove(archive, ec);
    const std::string tree = unpacked_tree_refusal(staging.u8string());
    if (!tree.empty()) {
        fs::remove_all(staging, ec);
        return fail("the unpacked archive is refused: " + tree);
    }
    std::string err;
    if (!write_pack_marker(staging.u8string(), InstalledPack{pack.id, feed_version, pack.asset.sha256}, err)) {
        fs::remove_all(staging, ec);
        return fail(err);
    }
    // Ready, under the name a start moves into place (finish_pending_packs); then moved now, which
    // fails only where the old copy is in use (Windows), and the next start finishes it.
    const fs::path pending = root / fs::u8path(".pending-" + pack.id);
    fs::remove_all(pending, ec);
    fs::rename(staging, pending, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        return fail("could not put the unpacked pack in place: " + ec.message());
    }
    const fs::path dest = root / fs::u8path(pack.id);
    const fs::path old = root / fs::u8path(".old-" + pack.id);
    bool now = true;
    if (fs::exists(dest, ec)) {
        fs::remove_all(old, ec);
        fs::rename(dest, old, ec);
        now = !ec;
    }
    if (now) {
        fs::rename(pending, dest, ec);
        now = !ec;
        fs::remove_all(old, ec);
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        restart_required_ = true;
    }
    publish(kind, pack.id, file, pack.asset.size, pack.asset.size, "done", "",
            json{{"restart_required", true}, {"in_place", now}});
}

void UpdateService::run_update_download(const FeedAsset & asset, std::string version) {
    std::error_code ec;
    const fs::path dir = fs::u8path(updates_dir());
    fs::create_directories(dir, ec);
    const fs::path final_path = dir / fs::u8path(asset.file_name());
    const fs::path part = dir / fs::u8path(asset.file_name() + ".part");
    if (!fetch_checked("update", "update", asset, part.u8string())) return;
    fs::remove(final_path, ec);
    fs::rename(part, final_path, ec);
    if (ec) {
        publish("update", "update", asset.file_name(), asset.size, asset.size, "error",
                "could not rename the finished file: " + ec.message());
        return;
    }
    publish("update", "update", asset.file_name(), asset.size, asset.size, "done", "",
            json{{"path", final_path.u8string()}, {"version", version}});
}

} // namespace bmoe::server
