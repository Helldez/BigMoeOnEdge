// os.h on Windows: the notification-area icon (Shell_NotifyIcon) with Open, Restart Server and
// Quit, closing to it, balloon notifications from it, the folder dialog (IFileOpenDialog), WebView2's
// zoom and source, one instance per session (a named mutex), and the installer run by the shell.
#include "os.h"

#include "platform.h"

#include "webview/webview.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <WebView2.h>

#include <cstdio>
#include <string>
#include <vector>

int app_main(int argc, char ** argv);

namespace bmoe::desktop::os {

const char * const kServerExecutable = "bmoe-server.exe";

namespace {

constexpr UINT kTrayMessage = WM_APP + 0x42;
constexpr UINT_PTR kSubclassId = 0x424d6f45; // "BMoE"
constexpr UINT kTrayId = 1;
enum : UINT { kCmdOpen = 1, kCmdRestart = 2, kCmdQuit = 3 };
constexpr const wchar_t * kTitle = L"BigMoeOnEdge";
constexpr const wchar_t * kInstanceMutex = L"Local\\BigMoeOnEdge.Desktop";

Hooks g_hooks;
NOTIFYICONDATAW g_nid{};
bool g_tray = false;
UINT g_taskbar_created = 0;

std::wstring widen(const std::string & s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
    return w;
}

std::string narrow(const wchar_t * w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

HWND hwnd_of(webview::webview & w) {
    const auto r = w.window();
    return r.ok() ? static_cast<HWND>(r.value()) : nullptr;
}

ICoreWebView2Controller * controller_of(webview::webview & w) {
    const auto r = w.browser_controller();
    return r.ok() ? static_cast<ICoreWebView2Controller *>(r.value()) : nullptr;
}

void add_tray_icon() {
    g_tray = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    if (g_tray) Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

void tray_menu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kCmdOpen, L"Open BigMoeOnEdge");
    AppendMenuW(m, MF_STRING, kCmdRestart, L"Restart Server");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kCmdQuit, L"Quit BigMoeOnEdge");
    POINT pt;
    GetCursorPos(&pt);
    // The menu closes when the person clicks elsewhere only if the window is foreground first.
    SetForegroundWindow(hwnd);
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == kCmdOpen && g_hooks.open) g_hooks.open();
    if (cmd == kCmdRestart && g_hooks.restart) g_hooks.restart();
    if (cmd == kCmdQuit && g_hooks.quit) g_hooks.quit();
}

// Ahead of the webview library's own window procedure: the close button, the tray icon's
// messages, and the end of the session.
LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_CLOSE) {
        if (g_hooks.close_requested && g_hooks.close_requested()) return 0;
    } else if (msg == kTrayMessage) {
        // NOTIFYICON_VERSION_4: the event in the low word.
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            if (g_hooks.open) g_hooks.open();
            return 0;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            tray_menu(hwnd);
            return 0;
        default:
            return 0;
        }
    } else if (msg == g_taskbar_created && g_taskbar_created != 0) {
        // Explorer restarted and forgot every icon: put this one back.
        add_tray_icon();
    } else if (msg == WM_ENDSESSION && wp) {
        if (g_hooks.quit) g_hooks.quit();
        return 0;
    } else if (msg == WM_DESTROY) {
        if (g_tray) Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_tray = false;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

} // namespace

bool attach(webview::webview & w, const Hooks & hooks) {
    g_hooks = hooks;
    HWND hwnd = hwnd_of(w);
    if (!hwnd) return false;
    SetWindowSubclass(hwnd, subclass_proc, kSubclassId, 0);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = kTrayId;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = kTrayMessage;
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    // The executable's own icon when it carries one, the system's application icon otherwise.
    g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_nid.szTip, kTitle, _TRUNCATE);
    add_tray_icon();
    return g_tray;
}

void show_window(webview::webview & w) {
    HWND hwnd = hwnd_of(w);
    if (!hwnd) return;
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd);
}

void hide_window(webview::webview & w) {
    if (HWND hwnd = hwnd_of(w)) ShowWindow(hwnd, SW_HIDE);
}

std::string current_url(webview::webview & w) {
    ICoreWebView2Controller * c = controller_of(w);
    if (!c) return "";
    ICoreWebView2 * core = nullptr;
    if (FAILED(c->get_CoreWebView2(&core)) || !core) return "";
    LPWSTR uri = nullptr;
    std::string out;
    if (SUCCEEDED(core->get_Source(&uri)) && uri) out = narrow(uri);
    CoTaskMemFree(uri);
    core->Release();
    return out;
}

void set_zoom(webview::webview & w, double scale) {
    if (ICoreWebView2Controller * c = controller_of(w)) c->put_ZoomFactor(scale);
}

std::string pick_folder(webview::webview & w) {
    IFileOpenDialog * dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return "";
    std::string out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (SUCCEEDED(dlg->Show(hwnd_of(w)))) {
        IShellItem * item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out = narrow(path);
            CoTaskMemFree(path);
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

void notify(const std::string & title, const std::string & body) {
    // A balloon from the notification-area icon, which Windows 10 and 11 show as a toast.
    if (!g_tray) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, widen(title).c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, widen(body).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

bool run_installer(const std::string & path, std::string & err) {
    // The setup program closes nothing of this app's: the app quits right after, and the setup's
    // own check for running copies waits for that.
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR) r <= 32) {
        err = "Windows could not start " + path;
        return false;
    }
    return true;
}

bool claim_single_instance(const std::string & data_dir) {
    (void) data_dir;
    // Kept open for the life of the process; the system closes it at the end.
    CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;
    // The copy already running is the app the person meant: its window comes forward.
    if (HWND other = FindWindowW(L"webview", kTitle)) {
        ShowWindow(other, IsIconic(other) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(other);
    }
    return false;
}

std::string resources_dir() {
    return "";
}

} // namespace bmoe::desktop::os

// A windowed program: no console, so --version and --help write to the terminal that started it,
// when there is one.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE * f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    int argc = 0;
    LPWSTR * wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(bmoe::desktop::os::narrow(wargv[i]));
    LocalFree(wargv);
    std::vector<char *> argv;
    for (std::string & a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    return app_main(argc, argv.data());
}
