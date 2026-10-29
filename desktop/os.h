// What the desktop app needs from each operating system, beyond what the webview library and the
// server's platform library (server/platform.h) already give: the window's close button, the tray
// or menu-bar item, notifications, the folder dialog, the page shown, zoom, one instance, running
// an installer. One file per system implements it (os_macos.mm, os_windows.cpp, os_linux.cpp);
// nothing else in desktop/ has a platform #if.
#pragma once

#include "webview/webview.h"

#include <functional>
#include <string>

namespace bmoe::desktop::os {

// The server's executable name beside the app.
extern const char * const kServerExecutable;

// What the system side calls back into the app with, always on the UI thread.
struct Hooks {
    std::function<void()> open;    // the tray's or the Dock's "Open"
    std::function<void()> restart; // "Restart Server": a server that keeps stopping waits for this
    std::function<void()> quit;    // "Quit", from any menu, and the system asking the app to end
    // The window's close button: true keeps the app running with the window hidden; false lets the
    // close go on, which quits.
    std::function<bool()> close_requested;
};

// Called once the window exists. Installs the menus (the macOS menu bar, whose Edit menu is what
// makes copy and paste work in the page), the tray item where the system has one, and the hooks.
// Returns whether there is a tray item, which is what closing to it needs.
bool attach(webview::webview & w, const Hooks & hooks);

// Shows the window and brings the app forward; and hides it, the app still running.
void show_window(webview::webview & w);
void hide_window(webview::webview & w);

// The URL of the page the window shows now, or "" when it cannot be read.
std::string current_url(webview::webview & w);

// The page's zoom, 1 for none.
void set_zoom(webview::webview & w, double scale);

// The system's folder dialog over the window; "" when the person cancelled.
std::string pick_folder(webview::webview & w);

// A desktop notification. Best effort: a system that has none, or a person who turned them off in
// the system's settings, gets nothing.
void notify(const std::string & title, const std::string & body);

// Runs a downloaded installer the way the system opens one (an installer program, a disk image, a
// new AppImage in place of the old one), with the app about to quit. False, with the reason, when
// it could not be started.
bool run_installer(const std::string & path, std::string & err);

// Takes this user's one-instance lock for the app, kept until the process ends. False when another
// copy holds it: that copy is asked to show its window, and this one should exit.
bool claim_single_instance(const std::string & data_dir);

// Where a bundle keeps the app's data apart from its code (an app bundle's Contents/Resources), or
// "" where the files sit beside the executable.
std::string resources_dir();

} // namespace bmoe::desktop::os
