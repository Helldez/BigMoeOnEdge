#include "app_core.h"

#include <cpp-httplib/httplib.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace bmoe::desktop {

json prefs_to_json(const DesktopPrefs & p) {
    return json{{"closeToTray", p.close_to_tray},
                {"port", p.port},
                {"notifications", p.notifications},
                {"uiScale", p.ui_scale}};
}

bool merge_prefs(DesktopPrefs & p, const json & patch, std::string & err) {
    if (!patch.is_object()) {
        err = "preferences must be an object";
        return false;
    }
    DesktopPrefs next = p;
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const std::string & k = it.key();
        const json & v = it.value();
        if (k == "closeToTray" || k == "notifications") {
            if (!v.is_boolean()) {
                err = k + " must be true or false";
                return false;
            }
            (k == "closeToTray" ? next.close_to_tray : next.notifications) = v.get<bool>();
        } else if (k == "port") {
            if (!v.is_number_integer() ||
                (v.get<long long>() != 0 && (v.get<long long>() < kMinPort || v.get<long long>() > kMaxPort))) {
                err = "port must be 0 (the server's default) or from " + std::to_string(kMinPort) + " to " +
                      std::to_string(kMaxPort);
                return false;
            }
            next.port = (int) v.get<long long>();
        } else if (k == "uiScale") {
            if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < kMinUiScale ||
                v.get<double>() > kMaxUiScale) {
                err = "uiScale must be a number from " + json(kMinUiScale).dump() + " to " + json(kMaxUiScale).dump();
                return false;
            }
            next.ui_scale = v.get<double>();
        }
    }
    p = next;
    return true;
}

DesktopPrefs load_prefs(const std::string & path, std::string & warning) {
    DesktopPrefs p;
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return p;
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        std::string err;
        if (!merge_prefs(p, json::parse(ss.str()), err)) warning = path + ": " + err + "; using the defaults";
    } catch (const std::exception & e) {
        warning = path + " is not JSON (" + e.what() + "); using the defaults";
    }
    return p;
}

bool save_prefs(const std::string & path, const DesktopPrefs & p, std::string & err) {
    std::error_code ec;
    fs::create_directories(fs::u8path(path).parent_path(), ec);
    // Written aside and renamed over, so a crash mid-write never leaves half a file.
    const fs::path tmp = fs::u8path(path + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << prefs_to_json(p).dump(2) << "\n";
        if (!out) {
            err = "could not write " + tmp.u8string();
            return false;
        }
    }
    fs::rename(tmp, fs::u8path(path), ec);
    if (ec) {
        err = "could not replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

bool port_is_free(int port) {
    constexpr time_t kProbeTimeoutS = 1;
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
    const bool bound = s.bind_to_port("127.0.0.1", port);
    // httplib's destructor does not close a socket bound without listen_after_bind(); stop() does.
    s.stop();
    return bound;
}

int pick_port(int preferred, const std::function<bool(int)> & is_free) {
    for (int i = 0; i < kPortTries; ++i) {
        const int port = preferred + i;
        if (port > kMaxPort) break;
        if (is_free(port)) return port;
    }
    return 0;
}

std::string server_origin(int port) {
    return "http://127.0.0.1:" + std::to_string(port);
}

bool same_origin(const std::string & url, const std::string & origin) {
    if (url.size() < origin.size() || url.compare(0, origin.size(), origin) != 0) return false;
    if (url.size() == origin.size()) return true;
    const char next = url[origin.size()];
    return next == '/' || next == '?' || next == '#';
}

bool is_update_file(const std::string & path, const std::string & updates_dir) {
    if (path.empty() || updates_dir.empty()) return false;
    std::error_code ec;
    const fs::path file = fs::canonical(fs::u8path(path), ec);
    if (ec || !fs::is_regular_file(file, ec)) return false;
    const fs::path dir = fs::canonical(fs::u8path(updates_dir), ec);
    if (ec) return false;
    return file.parent_path() == dir;
}

ServerLaunch server_launch(const std::string & server_path,
                           const std::string & resources_dir,
                           const std::string & data_dir,
                           int port,
                           long parent_pid,
                           const std::vector<std::string> & extra_args) {
    ServerLaunch l;
    l.server_path = server_path;
    l.args = {"--port", std::to_string(port)};
    if (parent_pid > 0) {
        l.args.push_back("--parent-pid");
        l.args.push_back(std::to_string(parent_pid));
    }
    if (!data_dir.empty()) {
        l.args.push_back("--data-dir");
        l.args.push_back(data_dir);
    }
    // A bundle keeps code and data apart (macOS signs Contents/MacOS as code only): the server is
    // told where its UI and catalog went. Beside the executable, it finds them on its own.
    if (!resources_dir.empty()) {
        std::error_code ec;
        const fs::path res = fs::u8path(resources_dir);
        if (fs::exists(res / "ui" / "index.html", ec)) {
            l.args.push_back("--ui-dir");
            l.args.push_back((res / "ui").u8string());
        }
        if (fs::exists(res / "catalog" / "models.json", ec)) {
            l.args.push_back("--catalog");
            l.args.push_back((res / "catalog" / "models.json").u8string());
        }
        if (fs::is_directory(res / "skills", ec)) {
            l.args.push_back("--skills-dir");
            l.args.push_back((res / "skills").u8string());
        }
    }
    l.args.insert(l.args.end(), extra_args.begin(), extra_args.end());
    return l;
}

std::string js_string(const std::string & s) {
    return json(s).dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string bridge_script() {
    return R"JS((function () {
  if (window.bmoeDesktop) return;
  var api = {};
  ['info', 'pickFolder', 'notify', 'openExternal', 'setPrefs', 'restartServer', 'applyUpdate'].forEach(function (n) {
    api[n] = function () { return window['__bmoe_' + n].apply(null, arguments); };
  });
  Object.defineProperty(window, 'bmoeDesktop', { value: Object.freeze(api) });
  // Another site opens in the browser: the window shows this server's pages and nothing else.
  var elsewhere = function (href) {
    try {
      var u = new URL(href, location.href);
      return (u.protocol === 'http:' || u.protocol === 'https:') && u.origin !== location.origin ? u.href : null;
    } catch (e) { return null; }
  };
  document.addEventListener('click', function (e) {
    var a = e.target && e.target.closest ? e.target.closest('a[href]') : null;
    var href = a && elsewhere(a.getAttribute('href'));
    if (href) { e.preventDefault(); window.__bmoe_openExternal(href); }
  }, true);
  var open = window.open;
  window.open = function (url) {
    var href = url && elsewhere(String(url));
    if (href) { window.__bmoe_openExternal(href); return null; }
    return open.apply(window, arguments);
  };
})();)JS";
}

} // namespace bmoe::desktop
