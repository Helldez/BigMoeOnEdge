// bmoe-desktop: the native window around the web UI (docs/desktop-app.md). It starts bmoe-server
// as a child process on a port of its own, shows the UI the server serves, and adds what a browser
// cannot do as `window.bmoeDesktop`. No engine logic here, and no platform code: os.h is what each
// system implements, server/platform.h what the server already knows (the data folder, opening a
// URL, the build's target).
#include "app_core.h"
#include "os.h"
#include "supervisor.h"

#include "platform.h"
#include "server.h"

#include "bmoe/version.h"

#include "webview/webview.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
using namespace bmoe;
using namespace bmoe::desktop;

namespace {

constexpr const char * kAppTitle = "BigMoeOnEdge";
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 820;
// A server that ends on its own is started again, but not forever: a driver that ends it at every
// start would otherwise loop. Past this many ends in this long, the window says so and waits.
constexpr int kMaxAutoRestarts = 3;
constexpr std::chrono::seconds kAutoRestartWindow{120};

// One thing after another, off the UI thread: starting, restarting and stopping the server, which
// wait for it.
class Serial {
public:
    Serial() : t_([this] { loop(); }) {}
    ~Serial() { finish(); }
    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (done_) return;
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }
    // Drops what has not started and waits for what has.
    void finish() {
        {
            std::lock_guard<std::mutex> lk(m_);
            done_ = true;
            q_.clear();
        }
        cv_.notify_one();
        if (t_.joinable()) t_.join();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return done_ || !q_.empty(); });
                if (q_.empty()) return;
                fn = std::move(q_.front());
                q_.pop_front();
            }
            fn();
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool done_ = false;
    std::thread t_;
};

std::string page(const std::string & title, const std::string & body) {
    // Text only, escaped: an error carries the server's log, which is not markup.
    std::string esc;
    for (const char c : body) {
        if (c == '<')
            esc += "&lt;";
        else if (c == '>')
            esc += "&gt;";
        else if (c == '&')
            esc += "&amp;";
        else
            esc += c;
    }
    return "<!doctype html><meta charset=utf-8><title>" + std::string(kAppTitle) +
           "</title><style>body{font:15px system-ui,sans-serif;margin:3em;color:#222;background:#fafafa}"
           "@media(prefers-color-scheme:dark){body{color:#ddd;background:#1b1b1b}}"
           "pre{white-space:pre-wrap;font-size:12px;opacity:.8}</style><h2>" +
           title + "</h2><pre>" + esc + "</pre>";
}

struct Options {
    std::string data_dir; // --data-dir: handed on to the server too
    bool devtools = false;
    std::vector<std::string> server_args; // after --
};

class DesktopApp {
public:
    DesktopApp(webview::webview & w, Options opt)
        : w_(w), opt_(std::move(opt)),
          data_dir_(opt_.data_dir.empty() ? server::platform::default_data_dir() : opt_.data_dir),
          prefs_path_((fs::u8path(data_dir_) / kPrefsFile).u8string()),
          updates_dir_((fs::u8path(data_dir_) / "updates").u8string()),
          log_path_((fs::u8path(data_dir_) / "logs" / "server.log").u8string()),
          sup_(log_path_, [this](int status) { serial_.post([this, status] { on_server_exit(status); }); }) {
        std::string warning;
        prefs_ = load_prefs(prefs_path_, warning);
        if (!warning.empty()) std::fprintf(stderr, "bmoe-desktop: %s\n", warning.c_str());
    }

    void setup() {
        w_.set_title(kAppTitle);
        w_.set_size(kWindowWidth, kWindowHeight, WEBVIEW_HINT_NONE);
        w_.init(bridge_script());
        bind_all();
        os::Hooks hooks;
        hooks.open = [this] { os::show_window(w_); };
        hooks.quit = [this] { quit(); };
        hooks.restart = [this] {
            w_.set_html(page("Restarting…", "Starting the engine's server again."));
            serial_.post([this] {
                restarts_.clear();
                start_server();
            });
        };
        hooks.close_requested = [this] {
            if (has_tray_ && prefs().close_to_tray) {
                os::hide_window(w_);
                return true;
            }
            quitting_ = true;
            return false;
        };
        has_tray_ = os::attach(w_, hooks);
        w_.set_html(page("Starting…", "Starting the engine's server."));
        serial_.post([this] { start_server(); });
    }

    // After the window's loop has ended: nothing more is started, and the server is stopped.
    void shutdown() {
        quitting_ = true;
        sup_.stop();
        serial_.finish();
        sup_.stop();
    }

private:
    DesktopPrefs prefs() {
        std::lock_guard<std::mutex> lk(m_);
        return prefs_;
    }

    std::string origin() const { return server_origin(port_.load()); }

    void quit() {
        quitting_ = true;
        sup_.stop();
        w_.terminate();
    }

    // Runs on the serial thread.
    void start_server() {
        if (quitting_) return;
        sup_.stop();
        const int preferred = prefs().port ? prefs().port : server::kDefaultPort;
        const int port = pick_port(preferred, port_is_free);
        if (port == 0) {
            show_error("No free port",
                       "Ports " + std::to_string(preferred) + " to " + std::to_string(preferred + kPortTries - 1) +
                           " are all in use. Choose another port in Settings, or close what uses them.");
            return;
        }
        std::string notice;
        if (port != preferred)
            notice = "Port " + std::to_string(preferred) + " is in use, so the server is on port " +
                     std::to_string(port) + ".";
        {
            std::lock_guard<std::mutex> lk(m_);
            notice_ = notice;
        }
        const fs::path exe = fs::u8path(server::platform::executable_dir());
        const ServerLaunch launch =
            server_launch((exe / os::kServerExecutable).u8string(), os::resources_dir(), opt_.data_dir, port,
                          server::platform::current_pid(), opt_.server_args);
        std::string err;
        if (!sup_.start(launch, port, err)) {
            if (!quitting_) show_error("The engine's server did not start", err + "\nIts log: " + log_path_);
            return;
        }
        port_ = port;
        const std::string url = origin() + "/";
        const double scale = prefs().ui_scale;
        w_.dispatch([this, url, scale, notice] {
            w_.navigate(url);
            os::set_zoom(w_, scale);
            if (!notice.empty()) notify_now(kAppTitle, notice);
        });
    }

    // Runs on the serial thread, after a server ended that nobody stopped.
    void on_server_exit(int status) {
        if (quitting_) return;
        const auto now = std::chrono::steady_clock::now();
        while (!restarts_.empty() && now - restarts_.front() > kAutoRestartWindow)
            restarts_.pop_front();
        if ((int) restarts_.size() >= kMaxAutoRestarts) {
            show_error("The engine's server keeps stopping",
                       "It ended with status " + std::to_string(status) + " " + std::to_string(kMaxAutoRestarts) +
                           " times in a row. Restart it from the menu once the cause is fixed.\nIts log: " + log_path_);
            w_.dispatch([this] { notify_now(kAppTitle, "The engine's server keeps stopping."); });
            return;
        }
        restarts_.push_back(now);
        w_.dispatch([this] { w_.set_html(page("Restarting…", "The engine's server stopped; starting it again.")); });
        start_server();
    }

    void show_error(const std::string & title, const std::string & body) {
        w_.dispatch([this, title, body] { w_.set_html(page(title, body)); });
    }

    void notify_now(const std::string & title, const std::string & body) {
        if (prefs().notifications) os::notify(title, body);
    }

    json info() {
        std::lock_guard<std::mutex> lk(m_);
        return json{{"version", version()},
                    {"target", server::platform::build_target()},
                    {"prefs", prefs_to_json(prefs_)},
                    {"port", port_.load()},
                    {"url", server_origin(port_.load()) + "/"},
                    {"notice", notice_},
                    {"tray", has_tray_},
                    {"dataDir", data_dir_},
                    {"log", log_path_}};
    }

    // A binding: answered only for a page of the server's own origin, with the call's arguments as
    // the JSON array the page passed; a thrown error rejects the page's promise with its message.
    void bind(const std::string & name, std::function<json(const json & args)> fn) {
        w_.bind(
            "__bmoe_" + name,
            [this, name, fn](const std::string & id, const std::string & req, void *) {
                if (!same_origin(os::current_url(w_), origin())) {
                    w_.resolve(id, 1, js_string("bmoeDesktop." + name + " answers the app's own pages only"));
                    return;
                }
                try {
                    const json args = json::parse(req);
                    w_.resolve(id, 0, fn(args.is_array() ? args : json::array()).dump());
                } catch (const std::exception & e) {
                    w_.resolve(id, 1, js_string(e.what()));
                }
            },
            nullptr);
    }

    static std::string arg_string(const json & args, size_t i) {
        if (args.size() <= i || !args[i].is_string()) throw std::runtime_error("expected a string argument");
        return args[i].get<std::string>();
    }

    void bind_all() {
        bind("info", [this](const json &) { return info(); });
        bind("pickFolder", [this](const json &) {
            const std::string p = os::pick_folder(w_);
            return p.empty() ? json() : json(p);
        });
        bind("notify", [this](const json & a) {
            notify_now(arg_string(a, 0), a.size() > 1 && a[1].is_string() ? a[1].get<std::string>() : "");
            return json();
        });
        bind("openExternal", [](const json & a) {
            const std::string url = arg_string(a, 0);
            if (url.compare(0, 8, "https://") != 0 && url.compare(0, 7, "http://") != 0)
                throw std::runtime_error("only http and https links open in the browser");
            server::platform::open_url(url);
            return json();
        });
        bind("setPrefs", [this](const json & a) {
            if (a.empty() || !a[0].is_object()) throw std::runtime_error("expected an object of preferences");
            DesktopPrefs next;
            std::string err;
            {
                std::lock_guard<std::mutex> lk(m_);
                next = prefs_;
            }
            const int old_port = next.port;
            if (!merge_prefs(next, a[0], err)) throw std::runtime_error(err);
            if (!save_prefs(prefs_path_, next, err)) throw std::runtime_error(err);
            {
                std::lock_guard<std::mutex> lk(m_);
                prefs_ = next;
            }
            os::set_zoom(w_, next.ui_scale);
            // A new port is a new server: the page then moves to it.
            if (next.port != old_port) serial_.post([this] { start_server(); });
            return prefs_to_json(next);
        });
        // Answered once the server is up again, or not.
        w_.bind(
            "__bmoe_restartServer",
            [this](const std::string & id, const std::string &, void *) {
                if (!same_origin(os::current_url(w_), origin())) {
                    w_.resolve(id, 1, js_string("bmoeDesktop.restartServer answers the app's own pages only"));
                    return;
                }
                w_.set_html(page("Restarting…", "Starting the engine's server again."));
                // Asked for by a person: the count of ends on its own starts over.
                serial_.post([this] {
                    restarts_.clear();
                    start_server();
                });
                w_.resolve(id, 0, "true");
            },
            nullptr);
        bind("applyUpdate", [this](const json & a) {
            const std::string path = arg_string(a, 0);
            if (!is_update_file(path, updates_dir_))
                throw std::runtime_error("only an installer the server downloaded and checked can be run");
            std::string err;
            if (!os::run_installer(path, err)) throw std::runtime_error(err);
            w_.dispatch([this] { quit(); });
            return json(true);
        });
    }

    webview::webview & w_;
    Options opt_;
    std::string data_dir_, prefs_path_, updates_dir_, log_path_;
    std::mutex m_;
    DesktopPrefs prefs_;
    std::string notice_;
    std::atomic<int> port_{0};
    bool has_tray_ = false;
    std::atomic<bool> quitting_{false};
    std::deque<std::chrono::steady_clock::time_point> restarts_; // the serial thread's
    ServerSupervisor sup_;
    Serial serial_; // last: its thread is the first thing stopped
};

void print_usage() {
    std::printf("usage: bmoe-desktop [--data-dir PATH] [--devtools] [-- bmoe-server options]\n"
                "\n"
                "  --data-dir PATH  settings and models (default: the per-user application data folder)\n"
                "  --devtools       allow the page's developer tools\n"
                "  --version        print the version\n"
                "  -- ...           everything after is passed to bmoe-server\n");
}

} // namespace

// The entry point each system's file calls (os_*.cpp: main, or WinMain), with UTF-8 arguments.
int app_main(int argc, char ** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--data-dir" && i + 1 < argc)
            opt.data_dir = argv[++i];
        else if (a == "--devtools")
            opt.devtools = true;
        else if (a == "--version") {
            std::printf("%s\n", version());
            return 0;
        } else if (a == "-h" || a == "--help") {
            print_usage();
            return 0;
        } else if (a == "--") {
            for (++i; i < argc; ++i)
                opt.server_args.push_back(argv[i]);
        } else if (a.compare(0, 5, "-psn_") == 0) {
            // macOS hands an app started from Finder a process serial number; nothing to do with it.
        } else {
            std::fprintf(stderr, "bmoe-desktop: unknown argument %s\n", a.c_str());
            print_usage();
            return 2;
        }
    }
    const std::string data_dir = opt.data_dir.empty() ? server::platform::default_data_dir() : opt.data_dir;
    std::error_code ec;
    fs::create_directories(fs::u8path(data_dir), ec);
    if (!os::claim_single_instance(data_dir)) return 0;
    try {
        webview::webview w(opt.devtools, nullptr);
        DesktopApp app(w, opt);
        app.setup();
        w.run();
        app.shutdown();
    } catch (const webview::exception & e) {
        std::fprintf(stderr, "bmoe-desktop: %s\n", e.what());
        return 1;
    }
    return 0;
}
