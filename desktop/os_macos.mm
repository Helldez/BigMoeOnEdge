// os.h on macOS: the menu bar (whose Edit menu is what makes copy and paste work in a WKWebView),
// a menu-bar status item, the Dock's reopen, closing to the menu bar, UNUserNotificationCenter,
// NSOpenPanel, and a disk image opened in Finder as the update.
#include "os.h"

#include "platform.h"

#include "webview/webview.h"

#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>
#import <WebKit/WebKit.h>
#include <objc/runtime.h>

#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <sys/file.h>
#include <unistd.h>

namespace fs = std::filesystem;

int app_main(int argc, char ** argv);

namespace bmoe::desktop::os {

const char * const kServerExecutable = "bmoe-server";

namespace {

// The hooks, for the Objective-C methods added below, which have no other way to reach them.
Hooks g_hooks;
NSStatusItem * g_status = nil;

NSWindow * window_of(webview::webview & w) {
    const auto r = w.window();
    return r.ok() ? (__bridge NSWindow *) r.value() : nil;
}

WKWebView * webview_of(webview::webview & w) {
    const auto r = w.browser_controller();
    return r.ok() ? (__bridge WKWebView *) r.value() : nil;
}

NSString * ns(const std::string & s) {
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

} // namespace

} // namespace bmoe::desktop::os

// The menus' target: each item calls a hook.
@interface BmoeMenuTarget : NSObject
- (void)openWindow:(id)sender;
- (void)restartServer:(id)sender;
- (void)quitApp:(id)sender;
@end

@implementation BmoeMenuTarget
- (void)openWindow:(id)sender {
    (void) sender;
    if (bmoe::desktop::os::g_hooks.open) bmoe::desktop::os::g_hooks.open();
}
- (void)restartServer:(id)sender {
    (void) sender;
    if (bmoe::desktop::os::g_hooks.restart) bmoe::desktop::os::g_hooks.restart();
}
- (void)quitApp:(id)sender {
    (void) sender;
    if (bmoe::desktop::os::g_hooks.quit) bmoe::desktop::os::g_hooks.quit();
}
@end

namespace bmoe::desktop::os {

namespace {

BmoeMenuTarget * g_target = nil;

// Not under ARC: the webview library's header is plain Objective-C runtime code that ARC refuses.
// What is made here lives as long as the app, or is autoreleased into the menu that keeps it.
NSMenuItem * item(NSString * title, SEL action, NSString * key, id target) {
    NSMenuItem * i = [[[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key] autorelease];
    if (target) i.target = target;
    return i;
}

void build_main_menu() {
    NSMenu * bar = [[[NSMenu alloc] init] autorelease];
    NSString * name = @"BigMoeOnEdge";

    NSMenuItem * app_item = [[[NSMenuItem alloc] init] autorelease];
    NSMenu * app = [[[NSMenu alloc] initWithTitle:name] autorelease];
    [app addItem:item([@"About " stringByAppendingString:name], @selector(orderFrontStandardAboutPanel:), @"", nil)];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:item(@"Restart Server", @selector(restartServer:), @"", g_target)];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:item([@"Hide " stringByAppendingString:name], @selector(hide:), @"h", nil)];
    NSMenuItem * others = item(@"Hide Others", @selector(hideOtherApplications:), @"h", nil);
    others.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [app addItem:others];
    [app addItem:item(@"Show All", @selector(unhideAllApplications:), @"", nil)];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:item([@"Quit " stringByAppendingString:name], @selector(quitApp:), @"q", g_target)];
    app_item.submenu = app;
    [bar addItem:app_item];

    // Without these, the page's text fields take no Cmd-C, Cmd-V, Cmd-A or Cmd-Z: the key
    // equivalents are the menu's, and the first responder (the web view) does the work.
    NSMenuItem * edit_item = [[[NSMenuItem alloc] init] autorelease];
    NSMenu * edit = [[[NSMenu alloc] initWithTitle:@"Edit"] autorelease];
    [edit addItem:item(@"Undo", @selector(undo:), @"z", nil)];
    NSMenuItem * redo = item(@"Redo", @selector(redo:), @"z", nil);
    redo.keyEquivalentModifierMask = NSEventModifierFlagShift | NSEventModifierFlagCommand;
    [edit addItem:redo];
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItem:item(@"Cut", @selector(cut:), @"x", nil)];
    [edit addItem:item(@"Copy", @selector(copy:), @"c", nil)];
    [edit addItem:item(@"Paste", @selector(paste:), @"v", nil)];
    [edit addItem:item(@"Select All", @selector(selectAll:), @"a", nil)];
    edit_item.submenu = edit;
    [bar addItem:edit_item];

    NSMenuItem * window_item = [[[NSMenuItem alloc] init] autorelease];
    NSMenu * window = [[[NSMenu alloc] initWithTitle:@"Window"] autorelease];
    [window addItem:item(@"Minimize", @selector(performMiniaturize:), @"m", nil)];
    [window addItem:item(@"Close", @selector(performClose:), @"w", nil)];
    window_item.submenu = window;
    [bar addItem:window_item];
    NSApp.windowsMenu = window;

    NSApp.mainMenu = bar;
}

void build_status_item() {
    g_status = [[[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength] retain];
    NSImage * icon = nil;
    if (@available(macOS 11.0, *))
        icon = [NSImage imageWithSystemSymbolName:@"cpu" accessibilityDescription:@"BigMoeOnEdge"];
    if (icon) {
        [icon setTemplate:YES]; // `template` is a C++ keyword: no dot syntax here
        g_status.button.image = icon;
    } else {
        g_status.button.title = @"BMoE";
    }
    NSMenu * menu = [[[NSMenu alloc] init] autorelease];
    [menu addItem:item(@"Open BigMoeOnEdge", @selector(openWindow:), @"", g_target)];
    [menu addItem:item(@"Restart Server", @selector(restartServer:), @"", g_target)];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItem:item(@"Quit BigMoeOnEdge", @selector(quitApp:), @"", g_target)];
    g_status.menu = menu;
}

// The webview library's own delegates handle the window's end and the app's launch; the methods
// below are added to their classes, so the close button and the Dock reach the hooks too.
BOOL window_should_close(id, SEL, id) {
    return g_hooks.close_requested && g_hooks.close_requested() ? NO : YES;
}

BOOL app_should_handle_reopen(id, SEL, id, BOOL) {
    if (g_hooks.open) g_hooks.open();
    return NO;
}

NSUInteger app_should_terminate(id, SEL, id) {
    // Cmd-Q from the Dock, a logout: the app's own quit stops the server, then ends the loop.
    if (g_hooks.quit) g_hooks.quit();
    return NSTerminateCancel;
}

void add_method(id object, SEL sel, IMP imp, const char * types) {
    if (!object) return;
    Class cls = object_getClass(object);
    if (!class_addMethod(cls, sel, imp, types)) class_replaceMethod(cls, sel, imp, types);
}

} // namespace

bool attach(webview::webview & w, const Hooks & hooks) {
    g_hooks = hooks;
    g_target = [[BmoeMenuTarget alloc] init];
    build_main_menu();
    build_status_item();
    NSWindow * win = window_of(w);
    add_method(win.delegate, @selector(windowShouldClose:), (IMP) window_should_close, "c@:@");
    add_method(NSApp.delegate, @selector(applicationShouldHandleReopen:hasVisibleWindows:),
               (IMP) app_should_handle_reopen, "c@:@c");
    add_method(NSApp.delegate, @selector(applicationShouldTerminate:), (IMP) app_should_terminate, "Q@:@");
    // SIGTERM and Ctrl-C would end the process past the quit that stops the server: both are
    // routed through it.
    for (const int sig : {SIGTERM, SIGINT}) {
        signal(sig, SIG_IGN);
        dispatch_source_t src =
            dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t) sig, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(src, ^{
          if (g_hooks.quit) g_hooks.quit();
        });
        dispatch_resume(src);
    }
    [win center];
    return true;
}

void show_window(webview::webview & w) {
    NSWindow * win = window_of(w);
    [NSApp activateIgnoringOtherApps:YES];
    [win makeKeyAndOrderFront:nil];
}

void hide_window(webview::webview & w) {
    [window_of(w) orderOut:nil];
}

std::string current_url(webview::webview & w) {
    WKWebView * v = webview_of(w);
    NSString * s = v.URL.absoluteString;
    return s ? std::string(s.UTF8String) : std::string();
}

void set_zoom(webview::webview & w, double scale) {
    WKWebView * v = webview_of(w);
    if (@available(macOS 11.0, *)) v.pageZoom = scale;
}

std::string pick_folder(webview::webview & w) {
    (void) w;
    NSOpenPanel * panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = NO;
    panel.canCreateDirectories = YES;
    if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0) return "";
    return std::string(panel.URLs[0].path.UTF8String);
}

void notify(const std::string & title, const std::string & body) {
    // The notification center belongs to a bundle: a bare binary (a build tree) has none to post
    // as, and asking for it would end the process.
    if (!NSBundle.mainBundle.bundleIdentifier) return;
    UNUserNotificationCenter * center = [UNUserNotificationCenter currentNotificationCenter];
    NSString *t = ns(title), *b = ns(body);
    [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound
                          completionHandler:^(BOOL granted, NSError *) {
                            if (!granted) return;
                            UNMutableNotificationContent * c =
                                [[[UNMutableNotificationContent alloc] init] autorelease];
                            c.title = t;
                            c.body = b;
                            UNNotificationRequest * r =
                                [UNNotificationRequest requestWithIdentifier:[[NSUUID UUID] UUIDString]
                                                                     content:c
                                                                     trigger:nil];
                            [center addNotificationRequest:r withCompletionHandler:nil];
                          }];
}

bool run_installer(const std::string & path, std::string & err) {
    // A disk image opens in Finder, where the new app is dragged over the old one once this one
    // has quit.
    if (![[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:ns(path)]]) {
        err = "macOS could not open " + path;
        return false;
    }
    return true;
}

bool claim_single_instance(const std::string & data_dir) {
    // Held for the life of the process: the system lets go of it when the process ends, however.
    const std::string lock = (fs::u8path(data_dir) / "desktop.lock").u8string();
    const int fd = open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return true; // no lock to take is no reason not to start
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return true;
    close(fd);
    // The other copy, brought forward: it is the app the person meant to open.
    NSString * bid = NSBundle.mainBundle.bundleIdentifier;
    if (bid)
        for (NSRunningApplication * a in [NSRunningApplication runningApplicationsWithBundleIdentifier:bid])
            if (a.processIdentifier != getpid()) [a activateWithOptions:NSApplicationActivateAllWindows];
    std::fprintf(stderr, "bmoe-desktop: already running for this user\n");
    return false;
}

std::string resources_dir() {
    const fs::path exe = fs::u8path(server::platform::executable_dir());
    std::error_code ec;
    if (exe.filename() == "MacOS" && exe.parent_path().filename() == "Contents") {
        const fs::path res = exe.parent_path() / "Resources";
        if (fs::is_directory(res, ec)) return res.u8string();
    }
    return "";
}

} // namespace bmoe::desktop::os

int main(int argc, char ** argv) {
    @autoreleasepool {
        return app_main(argc, argv);
    }
}
