import BmoeKit
import Foundation
import UIKit

/// Conversations and the turn in flight. The server keeps no history: every request carries the
/// whole conversation and the engine reuses the leading tokens its KV cache already holds, so a
/// follow-up costs its own message and switching conversations costs a full prefill. What is kept
/// here is saved on the device, one file per conversation, and survives a restart.
@MainActor
final class ChatModel: ObservableObject {
    @Published private(set) var conversations: [Conversation] = []
    @Published private(set) var current = Conversation()
    /// The answer and the thinking span in flight; committed to `current` when the turn ends.
    @Published private(set) var answer = ""
    @Published private(set) var reasoning = ""
    @Published private(set) var streaming = false
    /// The last turn's failure, shown under it and decided on by its code.
    @Published var error: TurnError?

    private let engine: Engine
    private let store: ConversationStore
    private var turn: Task<Void, Never>?
    private var stopRequested = false

    init(engine: Engine) {
        self.engine = engine
        store = ConversationStore(directory: engine.paths.conversations)
        conversations = store.list()
    }

    /// The last message is the person's, unanswered: a turn failed, and it can be sent again as it is.
    var canRetry: Bool { !streaming && current.messages.last?.role == .user }
    var canRegenerate: Bool {
        guard !streaming, let last = current.messages.last else { return false }
        return last.role == .assistant && !last.isDecision
    }

    func newConversation() {
        guard !streaming else { return }
        current = Conversation()
        error = nil
    }

    func select(_ c: Conversation) {
        guard !streaming else { return }
        current = c
        error = nil
    }

    func delete(_ id: UUID) {
        store.delete(id)
        if current.id == id && !streaming { current = Conversation() }
        conversations = store.list()
    }

    /// Sends `text` as the next turn of the current conversation.
    func send(_ text: String) {
        let prompt = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !prompt.isEmpty, !streaming, checkReady() else { return }
        var c = current
        c.messages.append(ChatMessage(role: .user, content: prompt))
        stamp(&c)
        run(c)
    }

    /// Sends the conversation again as it stands, after a failed turn left the person's message last.
    func retry() {
        guard canRetry, checkReady() else { return }
        run(current)
    }

    /// Drops the last answer and asks again: the KV holds the prompt, so only the answer is paid.
    func regenerate() {
        guard canRegenerate, checkReady() else { return }
        var c = current
        c.messages.removeLast()
        stamp(&c)
        run(c)
    }

    /// A decision: the model picks one of `options` for `question` from one prefill and no decode.
    /// Shown as the question with its lettered options, then each option with its probability;
    /// never sent back to the model as history.
    func choose(_ question: String, options: [String]) {
        let q = question.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !q.isEmpty, options.count >= 2, !streaming, checkReady() else { return }
        let before = current
        var c = current
        c.messages.append(ChatMessage(role: .user, content: Decide.userText(q, options)))
        stamp(&c)
        begin(c)
        turn = Task {
            let background = beginBackgroundWork()
            defer { endBackgroundWork(background) }
            var committed = before
            do {
                let r = Decide.parse(try await engine.api.decide(Decide.requestBody(q, options)), options)
                if !r.cancelled {
                    committed = c
                    committed.messages.append(ChatMessage(role: .assistant, content: "", metrics: r.metrics, choices: r.scores,
                                                          best: r.best))
                    committed.updated = Date()
                }
            } catch {
                self.error = Self.turnError(error)
            }
            // A decision's question is not a conversation turn: without its answer it would go out
            // as history with the next message, so a failed or cancelled one leaves none behind.
            persist(committed)
            current = committed
            streaming = false
        }
    }

    /// Stops the turn in flight; what streamed so far is kept as the answer.
    func stop() {
        guard streaming else { return }
        stopRequested = true
        Task { await engine.cancel() }
    }

    // MARK: the turn

    private func stamp(_ c: inout Conversation) {
        c.updated = Date()
        if let name = engine.info?.model?.name { c.model = name }
    }

    private func begin(_ c: Conversation) {
        persist(c)
        current = c
        answer = ""
        reasoning = ""
        error = nil
        streaming = true
        stopRequested = false
        engine.resetLive()
    }

    private func run(_ conversation: Conversation) {
        begin(conversation)
        turn = Task {
            let background = beginBackgroundWork()
            defer { endBackgroundWork(background) }
            var finish: String?
            var summary: JSON?
            var failure: TurnError?
            do {
                for try await data in engine.api.chat(ChatProtocol.requestBody(conversation.messages)) {
                    guard let chunk = ChatProtocol.parseChunk(data) else { continue }
                    if let e = chunk.error { failure = e }
                    if chunk.reset {
                        answer = ""
                        reasoning = ""
                    }
                    if let t = chunk.content { answer += t }
                    if let t = chunk.reasoning { reasoning += t }
                    if let f = chunk.finish { finish = f }
                    if let s = chunk.summary { summary = s }
                }
            } catch {
                if !stopRequested { failure = Self.turnError(error) }
            }
            let cancelled = finish == "cancelled" || stopRequested
            let metrics = summary.map { ChatProtocol.turnMetrics($0, nCtx: engine.info?.model?.nCtx ?? 0, cancelled: cancelled) } ?? ""
            // A turn that produced nothing and failed keeps the person's message, so it can be sent
            // again after the fix (a new chat, a loaded model); an empty cancelled turn is dropped.
            var committed = conversation
            if !answer.isEmpty || (failure == nil && !cancelled) {
                committed.messages.append(ChatMessage(role: .assistant, content: answer, reasoning: reasoning, metrics: metrics))
                committed.updated = Date()
                persist(committed)
            }
            current = committed
            answer = ""
            reasoning = ""
            error = failure
            streaming = false
        }
    }

    /// Nothing to send to: said with the code the server itself would answer, so one rule reads both.
    private func checkReady() -> Bool {
        if engine.ready { return true }
        error = engine.serverUp ? TurnError(message: "Load a model first, from Models.", code: APIError.noModel)
            : TurnError(message: "The engine is not running.", code: "")
        return false
    }

    private func persist(_ c: Conversation) {
        try? store.save(c)
        conversations = store.list()
    }

    private static func turnError(_ error: Error) -> TurnError {
        if let e = error as? APIError { return TurnError(message: e.message, code: e.code) }
        return TurnError(message: error.localizedDescription, code: "")
    }

    // A turn runs on in the background for the time the system allows, so an answer that is nearly
    // done when the person switches apps is not cut off and lost.
    private func beginBackgroundWork() -> UIBackgroundTaskIdentifier {
        UIApplication.shared.beginBackgroundTask(withName: "chat-turn") { [weak self] in
            Task { @MainActor in self?.stop() }
        }
    }

    private func endBackgroundWork(_ id: UIBackgroundTaskIdentifier) {
        if id != .invalid { UIApplication.shared.endBackgroundTask(id) }
    }
}
