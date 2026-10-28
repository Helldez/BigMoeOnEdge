import BmoeKit
import Foundation

/// The one engine, as the screens see it: what the server's event stream says, and the calls the
/// screens make. The server runs inside this app; this object starts it, connects to it on
/// loopback with the key it was started with, checks the server it reaches is that one, and keeps
/// reconnecting, so a screen never has to know whether the server is up, only what the state says.
///
/// Nothing here knows an engine parameter by name: the settings screen renders whatever
/// `GET /api/params` describes, and the plan screen whatever the planner decided.
@MainActor
final class Engine: ObservableObject {
    /// The server answers, and is the one this app started.
    @Published private(set) var serverUp = false
    /// Why it is not up: not started yet, stopped, ended, or a port someone else holds.
    @Published private(set) var serverError: String?
    /// Whether the running server listens on the network, with the shown key.
    @Published private(set) var shared = false
    @Published private(set) var info: ServerInfo?
    @Published private(set) var params: [ParamSpec] = []
    @Published private(set) var config: ConfigState?
    @Published private(set) var models = ModelsList.empty
    /// The server has listed the models once: until then an empty list says nothing.
    @Published private(set) var modelsListed = false
    @Published private(set) var downloads: [String: DownloadProgress] = [:]
    /// Per catalog entry, what a stopped download of it already has on the device: the row offers
    /// Resume with it, and Delete partial download.
    @Published private(set) var partials: [String: Int64] = [:]
    @Published private(set) var live = Telemetry()
    @Published private(set) var lastDone: JSON?
    /// The stage a measurement is in (Measure, or an auto load), from `plan_progress`; nil when none runs.
    @Published private(set) var planProgress: PlanProgress?
    /// The plan the server holds for the chosen model (`GET /api/plan?cached=1`, never a measurement):
    /// read when the server is reached, when a session opens and when a turn ends, so the Plan screen
    /// opens on it at once and its measured figures land when the answer does, whichever screen is open.
    @Published private(set) var plan: PlanResult?
    @Published private(set) var planError: String?
    /// A measurement asked for (`?measure=1`) is in flight: the person's tap, never automatic.
    @Published private(set) var measuring = false
    /// The engine's last diagnostics (`log` events), newest last.
    @Published private(set) var log: [String] = []

    let paths = AppPaths.current
    let preferences = Preferences()
    private let server = EngineServer()
    private let recorder: MetricsRecorder
    private let downloader: ModelDownloader
    private(set) var api: APIClient
    private var connector: Task<Void, Never>?
    private var restartAfterExit = false
    /// The conversations' events (`conversation`, `item`, `delta`): the chat's model reads them off
    /// the one event stream rather than holding a second one.
    var conversationEvents: ((String, JSON) -> Void)?
    /// The server was reached (again): what the chat keeps of the server is read anew.
    var connected: (() -> Void)?

    static let logLines = 200

    init() {
        recorder = MetricsRecorder(directory: paths.metrics)
        downloader = ModelDownloader(modelsDir: paths.documents, resumeDir: paths.downloads)
        // Until a launch says otherwise, a key no server has: nothing it reaches is taken for ours.
        api = APIClient(baseURL: server.baseURL, apiKey: APIClient.randomKey())
        downloader.onChange = { [weak self] progress in
            Task { @MainActor in self?.downloads = progress }
        }
        downloader.onLanded = { [weak self] in
            Task { @MainActor in await self?.refreshModels() }
        }
        downloader.reattach()
    }

    var ready: Bool { serverUp && info?.ready == true }
    var generating: Bool { info?.generating == true }
    var loading: Bool { info?.busyLoading == true }

    /// The loaded session's context against what a client of the server sends, worded with the
    /// parameter table's label for the setting that raises it.
    var contextAdvice: ContextAdvice? { ContextAdvice(model: info?.model, params: params) }

    /// The catalog entries whose transfers run.
    var downloading: Set<String> { Set(downloads.filter { $0.value.state == .running }.keys) }

    /// The key the server is shared on the network with: made once, kept in the keychain.
    var networkKey: String {
        if let k = Keychain.read(Preferences.networkKeyAccount), !k.isEmpty { return k }
        let k = APIClient.randomKey()
        Keychain.write(Preferences.networkKeyAccount, k)
        return k
    }

    // MARK: the server's life

    /// Starts the server if it is not running, and keeps the state following it.
    func start() {
        guard !server.running else { return }
        let share = preferences.shareOnNetwork
        // A fresh key per launch on loopback; the shown one when the network is served.
        let key = share ? networkKey : APIClient.randomKey()
        api = APIClient(baseURL: server.baseURL, apiKey: key)
        shared = share
        serverError = nil
        server.start(.init(share: share, key: key), paths: paths) { [weak self] code in
            self?.serverExited(code)
        }
        if connector == nil { connector = Task { await connectForever() } }
    }

    /// Stops the server (and frees the model). `start()` brings it back.
    func stop() {
        restartAfterExit = false
        recorder.close()
        if !server.stop() { serverError = "The engine is not running." }
    }

    /// A changed sharing setting: the server stops and starts again on the other address.
    func restart() {
        guard server.running else { return start() }
        restartAfterExit = true
        recorder.close()
        server.stop()
    }

    private func serverExited(_ code: Int32) {
        serverUp = false
        info = nil
        if restartAfterExit {
            restartAfterExit = false
            start()
            return
        }
        // Exit 1 at start is a port another program holds (the server says so on its log).
        serverError = code == 0 ? "The engine was stopped." : "The engine stopped (exit \(code))."
    }

    // MARK: the event stream

    private func connectForever() async {
        var backoff: UInt64 = 500_000_000
        while !Task.isCancelled {
            guard server.running else {
                try? await Task.sleep(nanoseconds: 500_000_000)
                continue
            }
            let a = api
            do {
                guard try await a.isOurs() else {
                    // Something answers on the port and it is not the server this launch started:
                    // a conversation is not handed to it.
                    serverError = "Port \(EngineServer.port) is held by a server this app did not start, so the engine "
                        + "cannot listen there. Close the program that holds it, then start the engine again."
                    try? await Task.sleep(nanoseconds: 5_000_000_000)
                    continue
                }
                for try await event in a.events() {
                    guard a === api else { break }
                    if !serverUp {
                        serverUp = true
                        serverError = nil
                        backoff = 500_000_000
                        Task { await refreshStatic() }
                        connected?()
                    }
                    handle(event)
                }
            } catch {
                // A closed port while the server starts or after it ended is not news; anything
                // else is what a person needs to read.
                if server.running, !(error is URLError) { serverError = "Engine not reachable: \(error.localizedDescription)" }
            }
            serverUp = false
            try? await Task.sleep(nanoseconds: backoff)
            backoff = min(backoff * 2, 5_000_000_000)
        }
    }

    /// What does not travel on the stream: the parameter table, the config and the models.
    private func refreshStatic() async {
        if let p = try? await api.params() { params = p }
        if let c = try? await api.config() { config = c }
        await refreshModels()
        await refreshPlan()
    }

    /// The plan held, without measuring; skipped while a measurement runs, which answers instead.
    func refreshPlan() async {
        guard !measuring else { return }
        do {
            plan = try await api.plan(measure: false)
            planError = nil
        } catch {
            planError = error.localizedDescription
        }
    }

    /// Measure the device for the chosen model: the probes run, and the plan shown is the new one.
    func measurePlan() async {
        guard !measuring else { return }
        measuring = true
        planError = nil
        do {
            plan = try await api.plan(measure: true)
        } catch {
            planError = error.localizedDescription
        }
        measuring = false
    }

    func refreshModels() async {
        if let m = try? await api.models() {
            models = m
            modelsListed = true
        }
        // The app downloads for itself here, so what a stopped transfer kept is its own to report.
        var p: [String: Int64] = [:]
        for e in models.catalog { if let got = downloader.partial(e) { p[e.id] = got } }
        partials = p
    }

    private func handle(_ e: SSEEvent) {
        guard let o = try? JSON(parsing: e.data) else { return }
        switch e.event {
        case "state":
            let next = ServerInfo(o)
            let before = info
            info = next
            if next.ready, before?.model?.path != next.model?.path || before?.model?.loadSeconds != next.model?.loadSeconds {
                // A new session: a fresh metrics file for it, and the models list may have changed.
                if preferences.metricsCSV { recorder.sessionOpened(info: next, config: config) }
                Task { await refreshModels() }
                // A new session runs a plan of its own (an auto load's, or the settings as they were).
                Task { await refreshPlan() }
            }
            if next.state == .empty || next.state == .error { recorder.close() }
        case "config":
            config = ConfigState(o)
        case "token":
            var t = live
            t.apply(token: o)
            // The threads the figures are read against belong to the session that produced them.
            if let m = info?.model {
                t.computeThreads = m.computeThreads
                t.busyThreads = m.busyThreads
            }
            if t.step <= 1 { t.avgTokensPerSecond = -1 }
            live = t
            recorder.token(o)
        case "done":
            var t = live
            t.apply(done: o)
            live = t
            lastDone = o
            recorder.done(o)
            // A finished turn is a new measurement beside the plan the model runs with.
            if info?.model != nil { Task { await refreshPlan() } }
        case "plan_progress":
            planProgress = PlanProgress(o)
        case "log":
            log.append("\(o["level"].string(or: "info")): \(o["message"].string(or: ""))")
            if log.count > Self.logLines { log.removeFirst(log.count - Self.logLines) }
        case "conversation", "item", "delta":
            conversationEvents?(e.event, o)
        default:
            break // downloads are the app's own here, not the server's
        }
    }

    /// The live card starts from zero at each generation.
    func resetLive() {
        live = Telemetry()
        lastDone = nil
    }

    // MARK: calls

    func load(_ path: String?) async throws { try await api.load(model: path) }
    func unload() async throws { try await api.unload() }
    func cancel() async { try? await api.cancel() }

    @discardableResult
    func setConfig(_ values: [String: JSON] = [:], reset: [String] = []) async throws -> ConfigState {
        let c = try await api.updateConfig(values: values, reset: reset)
        config = c
        return c
    }

    func setAutoPlan(_ on: Bool) async throws {
        config = try await api.updateConfig(autoPlan: on)
    }

    func applyPlan() async throws { config = try await api.applyPlan() }

    // MARK: models

    /// Starts or resumes the entry, refused before a byte moves when what it still needs does not
    /// fit in the space the device has free.
    func download(_ entry: CatalogEntry) throws {
        if let why = downloader.spaceRefusal(entry) { throw APIError(status: 507, message: "\(entry.title): \(why)") }
        downloader.start(entry)
    }
    func cancelDownload(_ id: String) { downloader.cancel(id) }
    func dismissDownload(_ id: String) { downloader.dismiss(id) }

    /// Throws away what stopped downloads of the entry kept: the app's own resume data, and through
    /// the server, by catalog id, any `.part` of it in the models directory.
    func deletePartial(_ entry: CatalogEntry) async throws {
        downloader.discard(entry)
        do {
            try await api.deletePartial(id: entry.id)
        } catch {
            await refreshModels()
            throw error
        }
        await refreshModels()
    }

    /// The system woke the app for the download session: the handler runs once its events are in.
    func handleBackgroundDownloads(_ completion: @escaping () -> Void) {
        downloader.backgroundEventsFinished = completion
        downloader.reattach()
    }

    /// Deletes a model of the models folder, named as the listing names it (`Library.deleteName`):
    /// the server removes every file it groups under it, a split set whole or not at all, and
    /// refuses the model loaded or loading. The screen offers no delete during the app's own
    /// download of it, which the server does not see.
    func delete(name: String) async throws {
        do {
            try await api.deleteModel(name: name)
        } catch {
            await refreshModels()
            throw error
        }
        await refreshModels()
    }

    /// Copies a file the person picked into the models folder, where the server lists it. The copy
    /// lands under a hidden name first and replaces a same-named model only once it is whole, so an
    /// import that fails (no space, the picked file goes away) leaves the model that was there. A
    /// file picked from the models folder itself is already in place.
    func importModel(from url: URL) async throws {
        let dest = paths.documents.appendingPathComponent(url.lastPathComponent)
        guard url.standardizedFileURL.resolvingSymlinksInPath() != dest.standardizedFileURL.resolvingSymlinksInPath()
        else { return }
        try await Task.detached(priority: .userInitiated) {
            let fm = FileManager.default
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            // Not a .gguf name, and hidden: the server never lists a copy in progress.
            let partial = dest.deletingLastPathComponent().appendingPathComponent(".\(dest.lastPathComponent).importing")
            if fm.fileExists(atPath: partial.path) { try fm.removeItem(at: partial) }
            do {
                try fm.copyItem(at: url, to: partial)
                if fm.fileExists(atPath: dest.path) {
                    _ = try fm.replaceItemAt(dest, withItemAt: partial)
                } else {
                    try fm.moveItem(at: partial, to: dest)
                }
            } catch {
                try? fm.removeItem(at: partial)
                throw error
            }
        }.value
        await refreshModels()
    }

    func hfSearch(_ query: String, token: String) async throws -> HFSearch { try await api.hfSearch(query: query, token: token) }
    func hfModels(_ repo: String, token: String) async throws -> HFModels { try await api.hfModels(repo: repo, token: token) }
    func hfInspect(_ repo: String, name: String, token: String) async throws -> HFFacts {
        try await api.hfInspect(repo: repo, name: name, token: token)
    }

    /// Adds the model to the catalog and starts its download, as the other entries download;
    /// answers the entry's id, whose row the screen scrolls to.
    @discardableResult
    func hfAdd(_ repo: String, name: String, token: String) async throws -> String {
        try await startAdded(try await api.hfAdd(repo: repo, name: name, token: token))
    }

    /// A direct https link to a gguf, made a catalog entry by the server (which checks the link
    /// and asks each shard's size), then downloaded here like any other entry from its `files`.
    @discardableResult
    func addURL(_ url: String) async throws -> String {
        try await startAdded(try await api.addURL(url))
    }

    private func startAdded(_ id: String) async throws -> String {
        await refreshModels()
        // An entry the app may not fetch (not https, a file name or a digest it refuses) is not
        // listed, so it is said rather than left waiting.
        guard let entry = models.catalog.first(where: { $0.id == id }) else {
            throw APIError(status: 0, message: "The server added the model, but its files are not ones this app can download.")
        }
        // A refusal (no space) throws back to the card that asked, which says why where the person looks.
        try download(entry)
        return id
    }

    func removeAdded(_ id: String) async throws {
        try await api.removeAdded(id: id)
        await refreshModels()
    }
}

/// The settings that are the app's own, never an engine parameter (docs/screens.md, Settings).
@MainActor
final class Preferences: ObservableObject {
    static let networkKeyAccount = "network-key"
    private let defaults = UserDefaults.standard

    /// Serve the engine on the local network, with the shown key, while the app is open.
    @Published var shareOnNetwork: Bool {
        didSet { defaults.set(shareOnNetwork, forKey: "shareOnNetwork") }
    }

    /// Write a per-token CSV for every loaded model, into Metrics.
    @Published var metricsCSV: Bool {
        didSet { defaults.set(metricsCSV, forKey: "metricsCSV") }
    }

    init() {
        shareOnNetwork = defaults.bool(forKey: "shareOnNetwork")
        metricsCSV = defaults.object(forKey: "metricsCSV") as? Bool ?? true
    }
}
