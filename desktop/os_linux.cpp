// os.h on Linux: GTK 3 under WebKitGTK. The tray through libayatana-appindicator3 when the build
// found it (BMOE_HAVE_APPINDICATOR); without it there is no tray and closing the window quits.
// Notifications through notify-send where it is installed, the folder dialog GTK's own, and an
// update is the new AppImage put in place of the running one and started once this one has quit.
#include "os.h"

#include "platform.h"

#include "webview/webview.h"

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#if defined(BMOE_HAVE_APPINDICATOR)
#include <libayatana-appindicator/app-indicator.h>
#endif

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <sys/file.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

int app_main(int argc, char ** argv);

namespace bmoe::desktop::os {

const char * const kServerExecutable = "bmoe-server";

namespace {

Hooks g_hooks;

GtkWindow * window_of(webview::webview & w) {
    const auto r = w.window();
    return r.ok() ? GTK_WINDOW(r.value()) : nullptr;
}

WebKitWebView * webview_of(webview::webview & w) {
    const auto r = w.browser_controller();
    return r.ok() ? WEBKIT_WEB_VIEW(r.value()) : nullptr;
}

gboolean on_delete(GtkWidget *, GdkEvent *, gpointer) {
    // TRUE stops the close: the window is hidden instead and the app keeps running.
    return g_hooks.close_requested && g_hooks.close_requested() ? TRUE : FALSE;
}

gboolean on_signal(gpointer) {
    if (g_hooks.quit) g_hooks.quit();
    return G_SOURCE_CONTINUE;
}

#if defined(BMOE_HAVE_APPINDICATOR)
void menu_item(GtkWidget * menu, const char * label, void (*fn)()) {
    GtkWidget * item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", G_CALLBACK(+[](GtkMenuItem *, gpointer f) { ((void (*)()) f)(); }),
                     (gpointer) fn);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

// The app's icon where a package put it (the AppImage's root, two folders above the program), or
// a themed one every desktop has.
std::string tray_icon() {
    const fs::path exe = fs::u8path(server::platform::executable_dir());
    std::error_code ec;
    for (const fs::path & p : {exe / "bigmoeonedge.png", exe.parent_path().parent_path() / "bigmoeonedge.png"})
        if (fs::exists(p, ec)) return p.u8string();
    return "utilities-system-monitor";
}
#endif

void spawn(const std::vector<std::string> & args) {
    std::vector<gchar *> argv;
    for (const std::string & a : args)
        argv.push_back(const_cast<gchar *>(a.c_str()));
    argv.push_back(nullptr);
    // No shell reads the arguments; the child is reaped by GLib.
    g_spawn_async(nullptr, argv.data(), nullptr,
                  GSpawnFlags(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL), nullptr,
                  nullptr, nullptr, nullptr);
}

} // namespace

bool attach(webview::webview & w, const Hooks & hooks) {
    g_hooks = hooks;
    GtkWindow * win = window_of(w);
    if (win) g_signal_connect(win, "delete-event", G_CALLBACK(on_delete), nullptr);
    // SIGTERM and Ctrl-C would end the process past the quit that stops the server.
    g_unix_signal_add(SIGTERM, on_signal, nullptr);
    g_unix_signal_add(SIGINT, on_signal, nullptr);
#if defined(BMOE_HAVE_APPINDICATOR)
    AppIndicator * ind =
        app_indicator_new("bigmoeonedge", tray_icon().c_str(), APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    GtkWidget * menu = gtk_menu_new();
    menu_item(menu, "Open BigMoeOnEdge", [] {
        if (g_hooks.open) g_hooks.open();
    });
    menu_item(menu, "Restart Server", [] {
        if (g_hooks.restart) g_hooks.restart();
    });
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_item(menu, "Quit BigMoeOnEdge", [] {
        if (g_hooks.quit) g_hooks.quit();
    });
    gtk_widget_show_all(menu);
    app_indicator_set_menu(ind, GTK_MENU(menu));
    app_indicator_set_title(ind, "BigMoeOnEdge");
    app_indicator_set_status(ind, APP_INDICATOR_STATUS_ACTIVE);
    return true;
#else
    return false;
#endif
}

void show_window(webview::webview & w) {
    if (GtkWindow * win = window_of(w)) {
        gtk_widget_show(GTK_WIDGET(win));
        gtk_window_present(win);
    }
}

void hide_window(webview::webview & w) {
    if (GtkWindow * win = window_of(w)) gtk_widget_hide(GTK_WIDGET(win));
}

std::string current_url(webview::webview & w) {
    WebKitWebView * v = webview_of(w);
    const gchar * uri = v ? webkit_web_view_get_uri(v) : nullptr;
    return uri ? std::string(uri) : std::string();
}

void set_zoom(webview::webview & w, double scale) {
    if (WebKitWebView * v = webview_of(w)) webkit_web_view_set_zoom_level(v, scale);
}

std::string pick_folder(webview::webview & w) {
    GtkWidget * dlg =
        gtk_file_chooser_dialog_new("Choose a folder", window_of(w), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Cancel",
                                    GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    std::string out;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gchar * path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) out = path;
        g_free(path);
    }
    gtk_widget_destroy(dlg);
    return out;
}

void notify(const std::string & title, const std::string & body) {
    // notify-send speaks the desktop's notification service; a system without it shows nothing.
    gchar * found = g_find_program_in_path("notify-send");
    if (!found) return;
    g_free(found);
    spawn({"notify-send", "--app-name=BigMoeOnEdge", title, body});
}

bool run_installer(const std::string & path, std::string & err) {
    std::error_code ec;
    fs::permissions(fs::u8path(path), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    std::string target = path;
    // An AppImage runs where it is: the new one takes the running one's place (APPIMAGE is where
    // the running one is, set by its runtime), copied when the two are on different disks.
    if (const char * running = std::getenv("APPIMAGE"); running && *running) {
        const fs::path dest = fs::u8path(running);
        const fs::path tmp = dest.parent_path() / (dest.filename().u8string() + ".new");
        fs::copy_file(fs::u8path(path), tmp, fs::copy_options::overwrite_existing, ec);
        if (!ec)
            fs::permissions(tmp,
                            fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                fs::perms::others_read | fs::perms::others_exec,
                            ec);
        if (!ec) fs::rename(tmp, dest, ec);
        if (ec) {
            err = "could not replace " + dest.u8string() + ": " + ec.message();
            return false;
        }
        target = dest.u8string();
    }
    // Started once this copy has quit and let go of the one-instance lock. The path is the shell's
    // $0, an argument, never part of the script.
    spawn({"/bin/sh", "-c", "sleep 2; exec \"$0\"", target});
    return true;
}

bool claim_single_instance(const std::string & data_dir) {
    // Held for the life of the process: the system lets go of it when the process ends, however.
    const std::string lock = (fs::u8path(data_dir) / "desktop.lock").u8string();
    const int fd = open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return true;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return true;
    close(fd);
    std::fprintf(stderr, "bmoe-desktop: already running for this user\n");
    return false;
}

std::string resources_dir() {
    return "";
}

} // namespace bmoe::desktop::os

int main(int argc, char ** argv) {
    return app_main(argc, argv);
}
