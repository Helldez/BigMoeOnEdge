import Foundation

/// What the migration needs of the server: create a conversation with its items, and delete one.
/// A protocol so a test hands it a fake server.
public protocol ConversationImporting: Sendable {
    /// `POST /api/conversations` with `body`; answers the server's whole answer.
    func importConversation(_ body: JSON) async throws -> JSON
    func deleteConversation(id: String) async throws
}

/// The conversations this app kept on the device before the server held them, moved to the server
/// once. Each one is posted with its messages as items; the local file goes only when the answer
/// shows the server kept every item, so a server that ignores `items` (older than this app) loses
/// nothing: the empty conversation it made is deleted again and the file stays for the next launch.
public enum ConversationMigration {
    public struct Outcome: Equatable, Sendable {
        public var moved = 0
        public var kept = 0
    }

    /// The items a local conversation becomes: the person's and the model's words. A decision's
    /// answer becomes text listing the options with their probabilities and the pick.
    public static func items(_ c: Conversation) -> [JSON] {
        c.messages.compactMap { m -> JSON? in
            switch m.role {
            case .system: return nil
            case .user: return ["kind": "user", "text": .string(m.content)]
            case .assistant:
                return ["kind": "assistant", "text": .string(text(m))]
            }
        }
    }

    static func text(_ m: ChatMessage) -> String {
        guard let choices = m.choices else { return m.content }
        var lines = choices.map { "\($0.label)) \($0.text): " + String(format: "%.0f%%", $0.probability * 100) }
        if choices.indices.contains(m.best) { lines.append("Picked \(choices[m.best].label)) \(choices[m.best].text)") }
        return lines.joined(separator: "\n")
    }

    public static func body(_ c: Conversation) -> JSON {
        ["title": .string(c.title), "items": .array(items(c))]
    }

    public static func run(store: ConversationStore, client: ConversationImporting) async -> Outcome {
        var out = Outcome()
        for c in store.list() {
            let sent = items(c)
            if sent.isEmpty {
                // Nothing said in it: nothing to move.
                store.delete(c.id)
                continue
            }
            do {
                let answer = try await client.importConversation(body(c))
                if AgentConversation.itemCount(answer) == sent.count {
                    store.delete(c.id)
                    out.moved += 1
                } else {
                    if let id = answer["id"].string, !id.isEmpty { try? await client.deleteConversation(id: id) }
                    out.kept += 1
                }
            } catch {
                out.kept += 1
            }
        }
        return out
    }
}
