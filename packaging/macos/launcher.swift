// The main executable of BigMoeOnEdge.app. bmoe-server has no window of its own, so this launcher
// is what the user sees while it runs: a Dock icon and a menu-bar item with "Open in Browser",
// "Show Log" and "Stop and Quit". Quitting the app, from either menu or the Dock, stops the server.
// The server, its libraries and its backend modules sit beside this executable in Contents/MacOS;
// the web UI and the catalog live in Contents/Resources and are handed over with --ui-dir and
// --catalog, since nothing but code may sit in Contents/MacOS of a signed bundle. The server keeps
// its own data folder (~/Library/Application Support/BigMoeOnEdge), the same one the archive uses.
//
// No engine logic here: this file only starts, watches and stops the process.
import AppKit

private let pageURL = URL(string: "http://127.0.0.1:8765/")!
private let probeURL = URL(string: "http://127.0.0.1:8765/api/info")!

final class Launcher: NSObject, NSApplicationDelegate {
    private var server: Process?
    private var statusItem: NSStatusItem?
    private var quitting = false
    private var termSource: DispatchSourceSignal?
    private let logURL = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Logs/BigMoeOnEdge/server.log")

    func applicationDidFinishLaunching(_ note: Notification) {
        // SIGTERM (kill, a logout that does not ask) would end this process without stopping the
        // server: route it through the normal quit, which does.
        signal(SIGTERM, SIG_IGN)
        let term = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        term.setEventHandler { NSApp.terminate(nil) }
        term.resume()
        termSource = term
        buildMainMenu()
        // One server per machine on the default port. If one already answers there (started from a
        // terminal, or by another copy of this app), show its page instead of failing to bind.
        if serverAnswers() {
            NSWorkspace.shared.open(pageURL)
            NSApp.terminate(nil)
            return
        }
        startServer()
        buildStatusItem()
    }

    // Clicking the Dock icon of the running app brings the page back.
    func applicationShouldHandleReopen(_ app: NSApplication, hasVisibleWindows: Bool) -> Bool {
        openPage()
        return false
    }

    func applicationWillTerminate(_ note: Notification) {
        quitting = true
        guard let p = server, p.isRunning else { return }
        p.terminate() // SIGTERM
        let deadline = Date().addingTimeInterval(5)
        while p.isRunning && Date() < deadline { usleep(50_000) }
        if p.isRunning { kill(p.processIdentifier, SIGKILL) }
    }

    private func serverAnswers() -> Bool {
        var req = URLRequest(url: probeURL)
        req.timeoutInterval = 1
        let done = DispatchSemaphore(value: 0)
        var ok = false
        URLSession.shared.dataTask(with: req) { _, resp, _ in
            ok = (resp as? HTTPURLResponse)?.statusCode == 200
            done.signal()
        }.resume()
        _ = done.wait(timeout: .now() + 2)
        return ok
    }

    private func startServer() {
        let exe = Bundle.main.executableURL!.deletingLastPathComponent().appendingPathComponent("bmoe-server")
        let res = Bundle.main.resourceURL!
        let fm = FileManager.default
        try? fm.createDirectory(at: logURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        fm.createFile(atPath: logURL.path, contents: nil)
        let log = try? FileHandle(forWritingTo: logURL)

        let p = Process()
        p.executableURL = exe
        p.arguments = ["--open",
                       "--ui-dir", res.appendingPathComponent("ui").path,
                       "--catalog", res.appendingPathComponent("catalog/models.json").path]
        p.standardInput = FileHandle.nullDevice
        p.standardOutput = log ?? FileHandle.nullDevice
        p.standardError = log ?? FileHandle.nullDevice
        p.terminationHandler = { [weak self] proc in
            DispatchQueue.main.async { self?.serverEnded(proc.terminationStatus) }
        }
        do {
            try p.run()
            server = p
        } catch {
            fail("bmoe-server could not be started: \(error.localizedDescription)")
        }
    }

    private func serverEnded(_ status: Int32) {
        if quitting { return }
        fail("bmoe-server stopped (exit status \(status)). Its output is in \(logURL.path).")
    }

    private func fail(_ text: String) {
        let alert = NSAlert()
        alert.messageText = "BigMoeOnEdge"
        alert.informativeText = text
        alert.addButton(withTitle: "Quit")
        alert.addButton(withTitle: "Show Log")
        NSApp.activate(ignoringOtherApps: true)
        if alert.runModal() == .alertSecondButtonReturn { showLog() }
        NSApp.terminate(nil)
    }

    @objc private func openPage() { NSWorkspace.shared.open(pageURL) }

    @objc private func showLog() { NSWorkspace.shared.activateFileViewerSelecting([logURL]) }

    @objc private func stopAndQuit() { NSApp.terminate(nil) }

    private func buildStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let icon = NSApp.applicationIconImage.copy() as? NSImage {
            icon.size = NSSize(width: 18, height: 18)
            item.button?.image = icon
        } else {
            item.button?.title = "BigMoeOnEdge"
        }
        item.button?.toolTip = "BigMoeOnEdge is running at \(pageURL.absoluteString)"
        item.menu = actionsMenu(header: true)
        statusItem = item
    }

    private func actionsMenu(header: Bool) -> NSMenu {
        let menu = NSMenu()
        if header {
            let running = NSMenuItem(title: "Running at \(pageURL.absoluteString)", action: nil, keyEquivalent: "")
            running.isEnabled = false
            menu.addItem(running)
            menu.addItem(.separator())
        }
        menu.addItem(withTitle: "Open in Browser", action: #selector(openPage), keyEquivalent: "o").target = self
        menu.addItem(withTitle: "Show Log", action: #selector(showLog), keyEquivalent: "l").target = self
        menu.addItem(.separator())
        menu.addItem(withTitle: "Stop and Quit", action: #selector(stopAndQuit), keyEquivalent: "q").target = self
        return menu
    }

    // The application menu, so Cmd-Q and the menu bar behave as in any Mac app.
    private func buildMainMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem()
        appItem.submenu = actionsMenu(header: false)
        main.addItem(appItem)
        NSApp.mainMenu = main
    }

    // The Dock icon's menu carries the same actions.
    func applicationDockMenu(_ sender: NSApplication) -> NSMenu? { actionsMenu(header: false) }
}

let app = NSApplication.shared
let delegate = Launcher()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
