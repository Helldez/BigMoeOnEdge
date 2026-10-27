import BmoeKit
import Foundation
import UIKit

/// A decision's question and the model's pick, shown under the conversation it was asked in. A
/// decision is not a conversation turn (docs/screens.md, "Choose"), so it is not sent to the
/// server's conversation and is not kept: it goes when another conversation opens.
struct DecisionCard: Identifiable, Equatable {
    let id = UUID()
    let question: String
    let result: Decide.Result
}

/// The agent as the screens see it (docs/agent.md): the server's conversations, the open one's
/// items as the event stream builds them, and the workspaces, skills, tasks and providers. The
/// server holds all of it and runs every turn; this object reads it, sends what the person does,
/// and follows the events, so a run goes on whether or not a screen is open.
@MainActor
final class AgentModel: ObservableObject {
    @Published private(set) var conversations: [ConversationSummary] = []
    /// The open conversation; nil for a new chat not sent yet.
    @Published private(set) var current: ConversationSummary?
    @Published private(set) var transcript: Transcript?
    @Published private(set) var loadingConversation = false
    /// What a new chat starts with; an open conversation's own are in `current`.
    @Published var draftMode = AgentMode.ask
    @Published var draftRoute = AgentRoute.local
    @Published var draftWorkspace = ""
    @Published private(set) var workspaces: [Workspace] = []
    @Published private(set) var providers: [Provider] = []
    @Published private(set) var skills: [Skill] = []
    @Published private(set) var tasks: [AgentTask] = []
    /// A message on its way to the server: Send waits for its answer.
    @Published private(set) var sending = false
    @Published var error: TurnError?
    @Published private(set) var decisions: [DecisionCard] = []
    @Published private(set) var deciding = false
    /// A line about the conversations moved from this device to the server, once.
    @Published var migrationNote: String?

    let engine: Engine
    let notifier = AgentNotifier()
    private var migrated = false
    private var background: UIBackgroundTaskIdentifier = .invalid

    init(engine: Engine) {
        self.engine = engine
        engine.agentEvents = { [weak self] event, data in self?.handle(event, data) }
        engine.connected = { [weak self] in Task { await self?.serverReached() } }
    }

    private var api: APIClient { engine.api }

    var mode: AgentMode { current?.mode ?? draftMode }
    var route: String { current?.route ?? draftRoute }
    var workspaceID: String { current?.workspace ?? draftWorkspace }
    var workspace: Workspace? { workspaces.first { $0.id == workspaceID } }
    /// The open conversation is running or waiting on an approval: Stop, not Send.
    var running: Bool { sending || (current?.busy ?? false) }
    var items: [AgentItem] { transcript?.items ?? [] }

    // MARK: the server

    private func serverReached() async {
        await refreshAll()
        if !migrated {
            migrated = true
            await migrate()
        }
    }

    func refreshAll() async {
        await refreshConversations()
        if let w = try? await api.workspaces() { workspaces = w }
        if let p = try? await api.providers() { providers = p }
        if let s = try? await api.skills() { skills = s }
        if let t = try? await api.tasks() { tasks = t }
        // A reconnect may have missed events of the open conversation: read it whole again.
        if let id = current?.id { await reload(id) }
    }

    func refreshConversations() async {
        if let c = try? await api.conversations() { conversations = c }
    }

    /// The conversations this app kept itself before the server held them, moved there once.
    private func migrate() async {
        let store = ConversationStore(directory: engine.paths.conversations)
        guard !store.list().isEmpty else { return }
        let out = await ConversationMigration.run(store: store, client: api)
        if out.moved > 0 {
            migrationNote = "Moved \(out.moved) saved \(out.moved == 1 ? "conversation" : "conversations") from this "
                + "device to the engine, where every conversation is now kept."
            await refreshConversations()
        }
        if out.kept > 0 {
            let n = out.kept
            migrationNote = (migrationNote.map { $0 + " " } ?? "")
                + "\(n) saved \(n == 1 ? "conversation stays" : "conversations stay") on this device for now: the engine did not take "
                + "\(n == 1 ? "it" : "them") whole, and the next start tries again."
        }
    }

    // MARK: conversations

    func newChat() {
        current = nil
        transcript = nil
        decisions = []
        error = nil
    }

    func open(_ id: String) {
        guard id != current?.id else { return }
        decisions = []
        error = nil
        current = conversations.first { $0.id == id } ?? ConversationSummary(id: id)
        transcript = Transcript(conversation: id)
        Task { await reload(id) }
    }

    private func reload(_ id: String) async {
        loadingConversation = transcript?.items.isEmpty ?? true
        defer { loadingConversation = false }
        do {
            let c = try await api.conversation(id: id)
            guard current?.id == id else { return }
            current = c.summary
            transcript = Transcript(conversation: id, items: c.items)
        } catch {
            if current?.id == id { self.error = Self.turnError(error) }
        }
    }

    func delete(_ id: String) {
        Task {
            do {
                try await api.deleteConversation(id: id)
                conversations.removeAll { $0.id == id }
                if current?.id == id { newChat() }
            } catch {
                self.error = Self.turnError(error)
            }
        }
    }

    func rename(_ id: String, to title: String) {
        Task {
            try? await api.updateConversation(id: id, title: title)
            await refreshConversations()
            if current?.id == id { current?.title = title }
        }
    }

    // MARK: the turn

    /// Sends `text`: into the open conversation, or into a new one made with the draft's mode,
    /// route and workspace. The answer is the person's item; the run reports on the stream.
    func send(_ text: String, think: Bool? = nil) {
        let message = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !message.isEmpty, !running, checkRoute() else { return }
        sending = true
        error = nil
        notifier.askOnce()
        holdBackground()
        Task {
            defer { sending = false }
            do {
                let id: String
                if let c = current {
                    id = c.id
                } else {
                    let made = try await api.createConversation(workspace: draftWorkspace, mode: draftMode, route: draftRoute)
                    id = made.summary.id
                    current = made.summary
                    transcript = Transcript(conversation: id, items: made.items)
                    conversations.removeAll { $0.id == id }
                    conversations.insert(made.summary, at: 0)
                }
                let item = try await api.sendMessage(conversation: id, text: message, think: think)
                if !item.id.isEmpty, transcript?.conversation == id { transcript?.upsert(item) }
                // The run is under way even if its first `conversation` event is still on its way.
                if current?.id == id, current?.state == "idle" { current?.state = "running" }
            } catch {
                self.error = Self.turnError(error)
                releaseBackground()
            }
        }
    }

    /// Drops the person's message `item` and everything after it, then sends `text` in its place.
    func edit(_ item: AgentItem, to text: String) {
        guard let id = current?.id, !running, checkRoute() else { return }
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty else { return }
        error = nil
        holdBackground()
        transcript?.truncate(from: item.id)
        Task {
            do {
                try await api.editMessage(conversation: id, item: item.id, text: t)
            } catch {
                self.error = Self.turnError(error)
                await reload(id)
                releaseBackground()
            }
        }
    }

    func answer(_ item: AgentItem, approve: Bool) {
        guard let id = current?.id else { return }
        Task {
            do {
                try await api.answerApproval(conversation: id, item: item.id, approve: approve)
            } catch {
                self.error = Self.turnError(error)
            }
        }
    }

    func stop() {
        guard let id = current?.id else { return }
        Task { try? await api.stopConversation(id: id) }
    }

    /// The local route needs the loaded model; a provider's does not. Said with the code the
    /// server itself would answer, so one rule reads both.
    private func checkRoute() -> Bool {
        guard engine.serverUp else {
            error = TurnError(message: "The engine is not running.", code: "")
            return false
        }
        if route == AgentRoute.local && !engine.ready {
            error = TurnError(message: "Load a model first, from Models.", code: APIError.noModel)
            return false
        }
        return true
    }

    // MARK: what the conversation runs with

    func setMode(_ m: AgentMode) {
        guard let id = current?.id else { return draftMode = m }
        current?.mode = m
        Task { try? await api.updateConversation(id: id, mode: m) }
    }

    func setRoute(_ r: String) {
        guard let id = current?.id else { return draftRoute = r }
        current?.route = r
        Task { try? await api.updateConversation(id: id, route: r) }
    }

    func setWorkspace(_ w: String) {
        guard let id = current?.id else { return draftWorkspace = w }
        current?.workspace = w
        Task { try? await api.updateConversation(id: id, workspace: w) }
    }

    // MARK: a decision

    /// The model picks one of `options` for `question` from one prefill and no decode; the card
    /// stays under the conversation until another one opens.
    func choose(_ question: String, options: [String]) {
        let q = question.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !q.isEmpty, options.count >= 2, !deciding else { return }
        guard engine.ready else {
            error = TurnError(message: "Load a model first, from Models.", code: APIError.noModel)
            return
        }
        deciding = true
        error = nil
        engine.resetLive()
        Task {
            defer { deciding = false }
            do {
                let r = Decide.parse(try await api.decide(Decide.requestBody(q, options)), options)
                if !r.cancelled { decisions.append(DecisionCard(question: Decide.userText(q, options), result: r)) }
            } catch {
                self.error = Self.turnError(error)
            }
        }
    }

    // MARK: the events

    private func handle(_ event: String, _ data: JSON) {
        switch event {
        case "conversation":
            let id = data["id"].string ?? ""
            let before = conversations.first { $0.id == id }?.state ?? current.flatMap { $0.id == id ? $0.state : nil }
            guard let row = ConversationList.apply(&conversations, event: data) else { return }
            if current?.id == id {
                current?.title = row.title
                current?.state = row.state
                current?.updated = row.updated
            }
            notifier.stateChanged(row, from: before)
            if !conversations.contains(where: \.busy) && !sending { releaseBackground() }
        case "item", "delta":
            transcript?.apply(event: event, data: data)
        case "task":
            let t = AgentTask(data)
            if let i = tasks.firstIndex(where: { $0.id == t.id }) { tasks[i] = t } else if !t.id.isEmpty { tasks.append(t) }
        default:
            break
        }
    }

    // A run goes on in the server, inside this app's process: iOS suspends the app soon after the
    // person leaves it, and the server with it. Asking for the time the system grants keeps a run
    // that is nearly done going (and its notification sent) when the person switches apps.
    private func holdBackground() {
        guard background == .invalid else { return }
        background = UIApplication.shared.beginBackgroundTask(withName: "agent-run") { [weak self] in
            Task { @MainActor in self?.releaseBackground() }
        }
    }

    private func releaseBackground() {
        guard background != .invalid else { return }
        UIApplication.shared.endBackgroundTask(background)
        background = .invalid
    }

    // MARK: workspaces

    /// Makes a folder in the app's own Workspaces folder (which the Files app shows) and adds it
    /// as a workspace: the server needs a real folder, and on a phone this is the one there is.
    func createWorkspace(named name: String) async throws -> Workspace {
        let folder = Self.folderName(name)
        guard !folder.isEmpty else { throw APIError(status: 0, message: "Give the folder a name.") }
        let url = engine.paths.workspaces.appendingPathComponent(folder, isDirectory: true)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        let w = try await api.addWorkspace(path: url.path, name: name.trimmingCharacters(in: .whitespaces))
        workspaces.removeAll { $0.id == w.id }
        workspaces.append(w)
        return w
    }

    /// Forgets the workspace; its folder and files stay where they are.
    func removeWorkspace(_ w: Workspace) async throws {
        try await api.removeWorkspace(id: w.id)
        workspaces.removeAll { $0.id == w.id }
        if draftWorkspace == w.id { draftWorkspace = "" }
    }

    /// A name a folder can carry: no path separators, no leading dot.
    static func folderName(_ name: String) -> String {
        let cleaned = name.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "/", with: "-").replacingOccurrences(of: ":", with: "-")
        return String(cleaned.drop { $0 == "." })
    }

    func preview(path: String) async throws -> PreviewFile {
        guard let id = current?.id else { throw APIError(status: 0, message: "No conversation is open.") }
        return try await api.file(conversation: id, path: path)
    }

    // MARK: skills, tasks, providers

    func refreshSkills() async { if let s = try? await api.skills() { skills = s } }
    func refreshTasks() async { if let t = try? await api.tasks() { tasks = t } }
    func refreshProviders() async { if let p = try? await api.providers() { providers = p } }
    func refreshWorkspaces() async { if let w = try? await api.workspaces() { workspaces = w } }

    func setSkill(_ s: Skill, enabled: Bool) async throws {
        try await api.setSkill(name: s.name, enabled: enabled)
        await refreshSkills()
    }

    func saveSkill(name: String, description: String, body: String, existing: String?) async throws {
        try await api.saveSkill(name: name, description: description, body: body, existing: existing)
        await refreshSkills()
    }

    func deleteSkill(_ s: Skill) async throws {
        try await api.deleteSkill(name: s.name)
        await refreshSkills()
    }

    func saveTask(_ t: AgentTask) async throws {
        try await api.saveTask(t)
        await refreshTasks()
    }

    func deleteTask(_ t: AgentTask) async throws {
        try await api.deleteTask(id: t.id)
        tasks.removeAll { $0.id == t.id }
    }

    func runTask(_ t: AgentTask) async throws {
        try await api.runTask(id: t.id)
        await refreshConversations()
    }

    func saveProvider(_ p: Provider, key: String) async throws {
        try await api.saveProvider(p, apiKey: key)
        await refreshProviders()
    }

    func deleteProvider(_ p: Provider) async throws {
        try await api.deleteProvider(id: p.id)
        await refreshProviders()
    }

    static func turnError(_ error: Error) -> TurnError {
        if let e = error as? APIError { return TurnError(message: e.message, code: e.code) }
        return TurnError(message: error.localizedDescription, code: "")
    }
}
