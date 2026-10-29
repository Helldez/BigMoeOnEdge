import Foundation

/// The conversation routes (docs/agent.md, "Routes"). Every call is a short exchange with the
/// server: an answer goes on in the server after `messages` answers, and streams on the event stream.
extension APIClient: ConversationImporting {
    static func escape(_ s: String) -> String { s.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? s }

    public func conversations() async throws -> [ConversationSummary] {
        ConversationSummary.list(try await get("/api/conversations", timeout: Self.quick))
    }

    public func createConversation(title: String? = nil) async throws -> ServerConversation {
        var body: [String: JSON] = [:]
        if let title, !title.isEmpty { body["title"] = .string(title) }
        return ServerConversation(try await send("POST", "/api/conversations", .object(body), timeout: Self.quick))
    }

    public func importConversation(_ body: JSON) async throws -> JSON {
        try await send("POST", "/api/conversations", body, timeout: Self.quick)
    }

    public func conversation(id: String) async throws -> ServerConversation {
        ServerConversation(try await get("/api/conversations/" + Self.escape(id), timeout: Self.quick))
    }

    public func renameConversation(id: String, title: String) async throws {
        _ = try await send("PATCH", "/api/conversations/" + Self.escape(id), ["title": .string(title)], timeout: Self.quick)
    }

    public func deleteConversation(id: String) async throws {
        _ = try await send("DELETE", "/api/conversations/" + Self.escape(id), nil, timeout: Self.quick)
    }

    /// Sends a message; answers the person's item at once (`202`), the answer streams on the stream.
    @discardableResult
    public func sendMessage(conversation id: String, text: String) async throws -> ConversationItem {
        ConversationItem(try await send("POST", "/api/conversations/\(Self.escape(id))/messages", ["text": .string(text)],
                                        timeout: Self.quick)["item"])
    }

    /// Drops `item` and everything after it, then sends `text` in its place: an edit, and Answer again.
    public func editMessage(conversation id: String, item: String, text: String) async throws {
        _ = try await send("POST", "/api/conversations/\(Self.escape(id))/edit", Transcript.editBody(item: item, text: text),
                           timeout: Self.quick)
    }

    /// Stops the answer; answers once it has stopped, which can take a moment.
    public func stopConversation(id: String) async throws {
        _ = try await send("POST", "/api/conversations/\(Self.escape(id))/stop", nil, timeout: Self.slow)
    }
}
