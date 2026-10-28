// Unit tests for the desktop app's own rules (desktop/app_core.h), with no window and no server:
// the preferences and their bounds, the port the server is started on, the arguments it gets, which
// pages the bridge answers, and which file may be run as an update.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "app_core.h"

#include <cpp-httplib/httplib.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace bmoe::desktop;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

static void prefs_tests() {
    DesktopPrefs p;
    std::string err;
    check(merge_prefs(p, json{{"closeToTray", false}, {"port", 9000}, {"notifications", false}, {"uiScale", 1.25}},
                      err) &&
              !p.close_to_tray && p.port == 9000 && !p.notifications && p.ui_scale == 1.25,
          "prefs: every field applies");
    const DesktopPrefs before = p;
    check(!merge_prefs(p, json{{"port", 80}}, err) && p.port == before.port, "prefs: a privileged port is refused");
    check(!merge_prefs(p, json{{"port", 70000}}, err), "prefs: a port past 65535 is refused");
    check(merge_prefs(p, json{{"port", 0}}, err) && p.port == 0, "prefs: port 0 is the server's default");
    check(!merge_prefs(p, json{{"uiScale", 10}}, err) && !merge_prefs(p, json{{"uiScale", 0.5}}, err),
          "prefs: a scale out of bounds is refused");
    check(!merge_prefs(p, json{{"closeToTray", "yes"}}, err), "prefs: a flag that is not a boolean is refused");
    p = before;
    check(!merge_prefs(p, json{{"notifications", true}, {"port", "x"}}, err) && !p.notifications,
          "prefs: a refused patch applies nothing of itself");
    check(merge_prefs(p, json{{"somethingNewer", 1}}, err), "prefs: a field from a newer app is ignored");
    check(!DesktopPrefs{}.serve_on_network, "prefs: the network is not served until asked");
    check(merge_prefs(p, json{{"serveOnNetwork", true}}, err) && p.serve_on_network &&
              prefs_to_json(p).value("serveOnNetwork", false),
          "prefs: serving the network is a preference");
    check(!merge_prefs(p, json::array(), err), "prefs: not an object");

    const fs::path dir = fs::temp_directory_path() / "bmoe_desktop_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const std::string path = (dir / kPrefsFile).u8string();
    std::string warning;
    const DesktopPrefs d = load_prefs(path, warning);
    check(d.close_to_tray && d.port == 0 && warning.empty(), "prefs: no file, the defaults and no warning");
    check(save_prefs(path, before, err), "prefs: saved: " + err);
    const DesktopPrefs back = load_prefs(path, warning);
    check(back.port == 9000 && !back.close_to_tray && back.ui_scale == 1.25, "prefs: read back as saved");
    std::ofstream(fs::u8path(path)) << "{not json";
    load_prefs(path, warning);
    check(!warning.empty(), "prefs: a broken file is said, and the defaults used");
    fs::remove_all(dir, ec);
}

static void port_tests() {
    const std::set<int> taken = {8765, 8766};
    check(pick_port(8765, [&](int p) { return !taken.count(p); }) == 8767, "port: the next free one after the taken");
    check(pick_port(8765, [](int) { return true; }) == 8765, "port: the preferred one when free");
    check(pick_port(8765, [](int) { return false; }) == 0, "port: none free in reach, none");
    check(pick_port(65535, [](int p) { return p != 65535; }) == 0, "port: never past 65535");
}

// The check leaves nothing bound: a socket it left listening would share the port with the server
// started on it, and the connections that land on it would never be answered.
static void port_check_tests() {
    int port = 0;
    {
        httplib::Server probe;
        port = probe.bind_to_any_port("127.0.0.1");
        probe.stop();
    }
    check(port > 0, "port check: a free port to try");
    check(port_is_free(port), "port check: a free port is free");
    check(port_is_free(port), "port check: still free after a check, so the check bound nothing it kept");
    httplib::Server held;
    held.Get("/", [](const httplib::Request &, httplib::Response & res) { res.set_content("x", "text/plain"); });
    check(held.bind_to_port("127.0.0.1", port), "port check: a server takes the port");
    std::thread serve([&] { held.listen_after_bind(); });
    held.wait_until_ready();
    check(!port_is_free(port), "port check: a port a server answers on is not free");
    held.stop();
    serve.join();
}

static void origin_tests() {
    const std::string o = server_origin(8765);
    check(o == "http://127.0.0.1:8765", "origin: the server's");
    check(same_origin("http://127.0.0.1:8765/", o) && same_origin("http://127.0.0.1:8765/#/chat", o) &&
              same_origin("http://127.0.0.1:8765", o) && same_origin("http://127.0.0.1:8765/?x=1", o),
          "origin: the server's pages");
    check(!same_origin("http://127.0.0.1:87650/", o) && !same_origin("http://127.0.0.1:8765.evil.org/", o) &&
              !same_origin("https://example.org/", o) && !same_origin("data:text/html,x", o) && !same_origin("", o) &&
              !same_origin("http://127.0.0.1:8766/", o),
          "origin: nothing else, not even a prefix of it");
}

static void update_file_tests() {
    const fs::path dir = fs::temp_directory_path() / "bmoe_desktop_test_updates";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "updates" / "sub");
    std::ofstream(dir / "updates" / "BigMoeOnEdge-setup.exe") << "x";
    std::ofstream(dir / "elsewhere.exe") << "x";
    std::ofstream(dir / "updates" / "sub" / "deeper.exe") << "x";
    const std::string u = (dir / "updates").u8string();
    check(is_update_file((dir / "updates" / "BigMoeOnEdge-setup.exe").u8string(), u), "update: a file of the folder");
    check(!is_update_file((dir / "elsewhere.exe").u8string(), u), "update: a file elsewhere is not");
    check(!is_update_file((dir / "updates" / ".." / "elsewhere.exe").u8string(), u), "update: nor one reached by ..");
    check(!is_update_file((dir / "updates" / "sub" / "deeper.exe").u8string(), u), "update: nor one in a subfolder");
    check(!is_update_file((dir / "updates" / "missing.exe").u8string(), u), "update: nor a missing one");
    check(!is_update_file(u, u), "update: nor the folder itself");
#if !defined(_WIN32)
    fs::create_symlink(dir / "elsewhere.exe", dir / "updates" / "link.exe", ec);
    check(!is_update_file((dir / "updates" / "link.exe").u8string(), u), "update: nor a link out of it");
#endif
    fs::remove_all(dir, ec);
}

static void network_key_tests() {
    const std::string a = new_network_key(), b = new_network_key();
    check(a.size() == 64 && a.find_first_not_of("0123456789abcdef") == std::string::npos && a != b,
          "key: 64 hex digits, a new one each time");
    const fs::path dir = fs::temp_directory_path() / "bmoe_desktop_test_key";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const std::string path = (dir / "network-key").u8string();
    std::string err;
    const std::string first = network_key(path, false, err);
    check(first.size() == 64 && network_key(path, false, err) == first, "key: made once, then kept");
    const std::string next = network_key(path, true, err);
    check(next.size() == 64 && next != first && network_key(path, false, err) == next, "key: replaced on request");
#if !defined(_WIN32)
    const fs::perms perms = fs::status(fs::u8path(path), ec).permissions();
    check((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
          "key: readable by this user only");
#endif
    fs::remove_all(dir, ec);
}

static void launch_tests() {
    const ServerLaunch l = server_launch("/app/bmoe-server", "", "", 8766, 42, "", {"--models-only"});
    check(l.server_path == "/app/bmoe-server" &&
              l.args == std::vector<std::string>{"--port", "8766", "--parent-pid", "42", "--models-only"},
          "launch: the port, the app's pid, then what the person passed");
    const fs::path res = fs::temp_directory_path() / "bmoe_desktop_test_res";
    std::error_code ec;
    fs::remove_all(res, ec);
    fs::create_directories(res / "ui");
    fs::create_directories(res / "catalog");
    fs::create_directories(res / "skills");
    std::ofstream(res / "ui" / "index.html") << "x";
    std::ofstream(res / "catalog" / "models.json") << "{}";
    const ServerLaunch n = server_launch("/app/bmoe-server", "", "", 8766, 0, "/data/network-key", {});
    check(n.args ==
              std::vector<std::string>{"--port", "8766", "--host", "0.0.0.0", "--api-key-file", "/data/network-key"},
          "launch: serving the network listens on every address behind the key's file");
    const ServerLaunch b = server_launch("/x/bmoe-server", res.u8string(), "/data", 8765, 0, "", {});
    check(b.args.size() == 10 && b.args[2] == "--data-dir" && b.args[3] == "/data" && b.args[4] == "--ui-dir" &&
              b.args[6] == "--catalog" && b.args[8] == "--skills-dir",
          "launch: a bundle's UI, catalog and skills, and the data folder the app was given");
    fs::remove_all(res, ec);
    const std::string js = bridge_script();
    bool all = true;
    for (const char * n :
         {"info", "pickFolder", "notify", "openExternal", "setPrefs", "restartServer", "newNetworkKey", "applyUpdate"})
        all = all && js.find(std::string("'") + n + "'") != std::string::npos;
    check(all && js.find("bmoeDesktop") != std::string::npos, "bridge: window.bmoeDesktop with every call");
    check(js_string("a\"b</script>") == "\"a\\\"b</script>\"", "js: a string literal, quotes escaped");
}

int main() {
    prefs_tests();
    port_tests();
    port_check_tests();
    origin_tests();
    update_file_tests();
    network_key_tests();
    launch_tests();
    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall desktop checks passed\n");
    return 0;
}
