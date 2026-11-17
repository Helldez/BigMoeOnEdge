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

/// The chat as the screens see it (docs/agent.md): the server's conversations and the open one's
/// items as the event stream builds them. The app starts its server with `--chat-only`, so they
/// are plain chat. The server holds them and writes every answer; this object reads them, sends
/// what the person does, and follows the events, so an answer goes on whether or not a screen is
/// open.
@MainActor
final class ChatModel: ObservableObject {
    @Published private(set) var conversations: [ConversationSummary] = []
    /// The open conversation; nil for a new chat not sent yet.
    @Published private(set) var current: ConversationSummary?
    /// A new chat's system prompt, sent when its first message makes the conversation.
    @Published private(set) var draftSystem = ""
    @Published private(set) var transcript: Transcript?
    @Published private(set) var loadingConversation = false
    /// A message on its way to the server: Send waits for its answer.
    @Published private(set) var sending = false
    @Published var error: TurnError?
    @Published private(set) var decisions: [DecisionCard] = []
    @Published private(set) var deciding = false
    /// A line about the conversations moved from this device to the server, once.
    @Published var migrationNote: String?
    /// The figures of each answer this app saw finish, by its item id, with the context it ran in:
    /// the line under the answer. The server keeps none, so an answer from before this launch has none.
    @Published private(set) var figures: [String: (summary: TurnSummary, nCtx: Int)] = [:]
    /// The last finished turn of the open conversation was stopped.
    @Published private(set) var lastStopped = false

    let engine: Engine
    private var migrated = false
    private var background: UIBackgroundTaskIdentifier = .invalid

    init(engine: Engine) {
        self.engine = engine
        engine.conversationEvents = { [weak self] event, data in self?.handle(event, data) }
        engine.connected = { [weak self] in Task { await self?.serverReached() } }
    }

    /// Throws as a closed port would while the engine's server has not said where it listens.
    private var api: APIClient {
        get throws { try engine.api }
    }

    /// An answer is being written in the open conversation: Stop, not Send.
    var running: Bool { sending || (current?.busy ?? false) }
    var items: [ConversationItem] { transcript?.items ?? [] }
    /// The answer that offers Answer again, if any.
    var answerAgainOn: String? { transcript?.answerAgainOn }

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
        // A stop between the server coming up and this leaves no client: tried again on the next start.
        guard let client = try? api else { return }
        let out = await ConversationMigration.run(store: store, client: client)
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

    /// The system prompt the person set: the open conversation's, else the new chat's.
    var system: String { current?.system ?? draftSystem }

    func newChat() {
        current = nil
        // A system prompt is the conversation's own: a new chat starts without one, as llama.cpp's web UI does.
        draftSystem = ""
        transcript = nil
        decisions = []
        error = nil
        lastStopped = false
    }

    func open(_ id: String) {
        guard id != current?.id else { return }
        decisions = []
        error = nil
        lastStopped = false
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

    /// The person's own system prompt for the open conversation, or the new chat's until its first
    /// message. Answers whether it was kept; `error` says why when it was not.
    func setSystem(_ text: String) async -> Bool {
        guard let id = current?.id else {
            draftSystem = text
            return true
        }
        do {
            let c = try await api.setSystemPrompt(id: id, text: text)
            if current?.id == id { current?.system = c.system }
            if let i = conversations.firstIndex(where: { $0.id == id }) { conversations[i].system = c.system }
            return true
        } catch {
            self.error = Self.turnError(error)
            return false
        }
    }

    func rename(_ id: String, to title: String) {
        Task {
            try? await api.renameConversation(id: id, title: title)
            await refreshConversations()
            if current?.id == id { current?.title = title }
        }
    }

    // MARK: the turn

    /// Why an image cannot go in a message now, or nil when it can: the loaded model reads images
    /// only with its projector loaded beside it (`GET /api/info`, `model.vision`). The composer
    /// shows the entries disabled with this, never hidden.
    var imageReason: String? {
        guard engine.ready, let model = engine.info?.model else {
            return "No model is loaded: one loaded with its projector file reads images"
        }
        return model.vision ? nil : "The loaded model does not read images: load one with its projector file"
    }

    /// Sends `text` with `images` (data URLs): into the open conversation, or into a new one. The
    /// server's answer is the person's item; the model's answer streams on the event stream.
    /// Answers whether the server took the message, so the composer keeps what it refused.
    @discardableResult
    func send(_ text: String, images: [String] = []) async -> Bool {
        let message = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !message.isEmpty || !images.isEmpty, !running, canSend() else { return false }
        sending = true
        error = nil
        lastStopped = false
        holdBackground()
        defer { sending = false }
        do {
                let id: String
                if let c = current {
                    id = c.id
                } else {
                    let made = try await api.createConversation(system: draftSystem)
                    draftSystem = ""
                    id = made.summary.id
                    current = made.summary
                    transcript = Transcript(conversation: id, items: made.items)
                    conversations.removeAll { $0.id == id }
                    conversations.insert(made.summary, at: 0)
                }
            let item = try await api.sendMessage(conversation: id, text: message, images: images)
            if !item.id.isEmpty, transcript?.conversation == id { transcript?.upsert(item) }
            // The answer is under way even if its first `conversation` event is still on its way.
            if current?.id == id, current?.state == "idle" { current?.state = "running" }
            return true
        } catch {
            self.error = Self.turnError(error)
            releaseBackground()
            return false
        }
    }

    /// Drops the person's message `item` and everything after it, then sends `text` in its place,
    /// with `images` (the item's own, as the web UI resends them: an edit changes what was asked,
    /// not what was shown).
    func edit(_ item: ConversationItem, to text: String, images: [String] = []) {
        guard let id = current?.id, !running, canSend() else { return }
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty || !images.isEmpty else { return }
        error = nil
        holdBackground()
        transcript?.truncate(from: item.id)
        Task {
            do {
                try await api.editMessage(conversation: id, item: item.id, text: t, images: images)
            } catch {
                self.error = Self.turnError(error)
                await reload(id)
                releaseBackground()
            }
        }
    }

    /// Answer again: the last question, resent as it was, as the web UI does.
    func answerAgain() {
        guard let last = transcript?.lastUser else { return }
        edit(last, to: last.text, images: last.images)
    }

    func stop() {
        guard let id = current?.id else { return }
        Task { try? await api.stopConversation(id: id) }
    }

    /// A message needs the server and the loaded model. Said with the code the server itself
    /// would answer, so one rule reads both.
    private func canSend() -> Bool {
        guard engine.serverUp else {
            error = TurnError(message: "The engine is not running.", code: "")
            return false
        }
        if !engine.ready {
            error = TurnError(message: "Load a model first, from Models.", code: APIError.noModel)
            return false
        }
        return true
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
            guard let row = ConversationList.apply(&conversations, event: data) else { return }
            if current?.id == id {
                current?.title = row.title
                current?.state = row.state
                current?.updated = row.updated
            }
            if !conversations.contains(where: \.busy) && !sending { releaseBackground() }
        case "item", "delta":
            transcript?.apply(event: event, data: data)
        case "done":
            // The engine's figures for the turn that just ended: the open conversation's newest
            // answer, while it is the one being written.
            guard current != nil, let answer = items.last(where: { $0.kind == .assistant }) else { return }
            let s = TurnSummary(data)
            figures[answer.id] = (s, engine.info?.model?.nCtx ?? 0)
            lastStopped = s.cancelled
        default:
            break
        }
    }

    // An answer is written in the server, inside this app's process: iOS suspends the app soon
    // after the person leaves it, and the server with it. Asking for the time the system grants
    // keeps an answer that is nearly done going when the person switches apps.
    private func holdBackground() {
        guard background == .invalid else { return }
        background = UIApplication.shared.beginBackgroundTask(withName: "chat-turn") { [weak self] in
            Task { @MainActor in self?.releaseBackground() }
        }
    }

    private func releaseBackground() {
        guard background != .invalid else { return }
        UIApplication.shared.endBackgroundTask(background)
        background = .invalid
    }

    static func turnError(_ error: Error) -> TurnError {
        if let e = error as? APIError { return TurnError(message: e.message, code: e.code) }
        return TurnError(message: error.localizedDescription, code: "")
    }
}
