#include "supervisor.h"

#define SUBPROCESS_HAVE_CWD 0
#include "sheredom/subprocess.h"

#include <cpp-httplib/httplib.h>

#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace bmoe::desktop {

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(250);
constexpr time_t kProbeTimeoutS = 1;
// Lines of the server's log a failure to start quotes.
constexpr size_t kLogTailLines = 12;

std::string log_tail(const std::string & path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    std::deque<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
        if (lines.size() > kLogTailLines) lines.pop_front();
    }
    std::string out;
    for (const std::string & l : lines)
        out += l + "\n";
    return out;
}

bool answers_info(int port) {
    httplib::Client c("127.0.0.1", port);
    c.set_connection_timeout(kProbeTimeoutS, 0);
    c.set_read_timeout(kProbeTimeoutS, 0);
    const auto res = c.Get("/api/info");
    return res && res->status == 200;
}

} // namespace

bool port_is_free(int port) {
    // Something answering there (on any address the port is bound on) is a no; so is a port this
    // process cannot bind, whoever holds it.
    {
        httplib::Client c("127.0.0.1", port);
        c.set_connection_timeout(kProbeTimeoutS, 0);
        c.set_read_timeout(kProbeTimeoutS, 0);
        const auto res = c.Get("/");
        if (res || res.error() != httplib::Error::Connection) return false;
    }
    httplib::Server s;
    return s.bind_to_port("127.0.0.1", port);
}

struct ServerSupervisor::Child {
    subprocess_s proc{};
};

ServerSupervisor::ServerSupervisor(std::string log_path, std::function<void(int)> on_exit)
    : log_path_(std::move(log_path)), on_exit_(std::move(on_exit)) {}

ServerSupervisor::~ServerSupervisor() {
    stop();
}

bool ServerSupervisor::start(const ServerLaunch & launch, int port, std::string & err) {
    stop();
    std::unique_lock<std::mutex> lk(m_);
    std::error_code ec;
    fs::create_directories(fs::u8path(log_path_).parent_path(), ec);
    // The previous run's log is kept beside this one: a crash is read after the restart.
    fs::rename(fs::u8path(log_path_), fs::u8path(log_path_ + ".1"), ec);

    std::vector<const char *> argv{launch.server_path.c_str()};
    for (const std::string & a : launch.args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);
    auto child = std::make_unique<Child>();
    // No console window for it on Windows, its output into the log, the app's environment.
    const int opts =
        subprocess_option_inherit_environment | subprocess_option_no_window | subprocess_option_combined_stdout_stderr;
    if (subprocess_create(argv.data(), opts, &child->proc) != 0) {
        err = "could not start " + launch.server_path;
        return false;
    }
    stopping_ = false;
    exited_ = false;
    running_ = true;
    Child * c = child.get();
    child_ = std::move(child);
    // Copies the server's output into its log until it ends, then reaps it; an end nobody asked
    // for is reported.
    reader_ = std::thread([this, c] {
        std::ofstream log(fs::u8path(log_path_), std::ios::binary | std::ios::trunc);
        // Line by line, so the log is current while the server runs (a block read would wait for
        // a full buffer).
        if (FILE * out = subprocess_stdout(&c->proc)) {
            char buf[4096];
            while (std::fgets(buf, sizeof(buf), out)) {
                log << buf;
                log.flush();
            }
        }
        int rc = 0;
        subprocess_join(&c->proc, &rc);
        status_ = rc;
        exited_ = true;
        running_ = false;
        if (!stopping_ && on_exit_) on_exit_(rc);
    });

    // Waited for without the lock, so a quit meanwhile stops it at once.
    lk.unlock();
    const auto deadline = std::chrono::steady_clock::now() + kServerStartTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stopping_) {
            err = "stopped while starting";
            return false;
        }
        if (exited_) {
            err = "bmoe-server stopped as it started (exit status " + std::to_string(status_) + "):\n" +
                  log_tail(log_path_);
            return false;
        }
        if (answers_info(port)) return true;
        std::this_thread::sleep_for(kPollInterval);
    }
    err = "bmoe-server did not answer on port " + std::to_string(port) + " in " +
          std::to_string(kServerStartTimeout.count()) + " s";
    return false;
}

void ServerSupervisor::stop() {
    std::lock_guard<std::mutex> lk(m_);
    if (!child_) return;
    stopping_ = true;
    if (!exited_) subprocess_terminate(&child_->proc);
    if (reader_.joinable()) reader_.join();
    subprocess_destroy(&child_->proc);
    child_.reset();
    running_ = false;
}

} // namespace bmoe::desktop
