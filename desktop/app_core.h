// The desktop app's own rules, apart from any window or process so they are tested on their own
// (tests/desktop_test.cpp): its preferences and their bounds, the port it starts the server on, the
// arguments the server gets, which pages may use the bridge, and which file may be run as an
// update. docs/desktop-app.md is the contract.
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace bmoe::desktop {

using json = nlohmann::ordered_json;

// The preferences' bounds. A port below 1024 needs privileges a person's app does not have; the
// scale is the range the UI's own setting offers, past which its layout no longer holds.
inline constexpr int kMinPort = 1024;
inline constexpr int kMaxPort = 65535;
inline constexpr double kMinUiScale = 0.9;
inline constexpr double kMaxUiScale = 1.5;
// How many ports after the preferred one are tried when it is taken.
inline constexpr int kPortTries = 20;
// The file the preferences are kept in, in the server's data folder.
inline constexpr const char * kPrefsFile = "desktop.json";

struct DesktopPrefs {
    bool close_to_tray = true; // closing the window keeps the app (and its server) running
    int port = 0;              // 0: the server's default port
    bool notifications = true;
    double ui_scale = 1.0;
};

json prefs_to_json(const DesktopPrefs & p);

// Apply the fields `patch` names to `p`. Unknown fields are ignored (an older app reading a newer
// file); a known one of the wrong type or out of bounds is refused, and nothing is applied.
bool merge_prefs(DesktopPrefs & p, const json & patch, std::string & err);

// The preferences in `path`, or the defaults when the file is missing or unreadable (with a
// warning for the latter, so a hand-edit gone wrong is said rather than lost silently).
DesktopPrefs load_prefs(const std::string & path, std::string & warning);
bool save_prefs(const std::string & path, const DesktopPrefs & p, std::string & err);

// The first port from `preferred` on that `is_free` accepts, trying kPortTries; 0 when none is.
int pick_port(int preferred, const std::function<bool(int)> & is_free);

// "http://127.0.0.1:<port>": the server's origin, which every page the bridge answers is on.
std::string server_origin(int port);

// Whether `url` is a page of `origin`: the same scheme, host and port, and nothing more. A prefix
// match would take "http://127.0.0.1:8765.evil" for the server.
bool same_origin(const std::string & url, const std::string & origin);

// Whether `path` is a file the server downloaded and checked as an update: a regular file directly
// inside `updates_dir`, after links and `..` are resolved. The bridge runs nothing else.
bool is_update_file(const std::string & path, const std::string & updates_dir);

// What the server is started with: `server_path` is bmoe-server beside the app; `resources_dir`,
// when the app is a bundle whose web UI, catalog and bundled skills sit apart from the code, holds
// ui/, catalog/ and skills/; `data_dir` is passed on when the app was given one; `parent_pid` is the app's, so the
// server stops when the app is gone however it ended.
struct ServerLaunch {
    std::string server_path;
    std::vector<std::string> args;
};
ServerLaunch server_launch(const std::string & server_path,
                           const std::string & resources_dir,
                           const std::string & data_dir,
                           int port,
                           long parent_pid,
                           const std::vector<std::string> & extra_args);

// The script every page of the window starts with: `window.bmoeDesktop`, over the native
// bindings, and links to other sites opened in the browser rather than in the window. The native
// side answers only pages of the server's origin (same_origin); this script is convenience, not
// the guard.
std::string bridge_script();

// A JSON string literal of `s`, for a value handed into a script.
std::string js_string(const std::string & s);

} // namespace bmoe::desktop
