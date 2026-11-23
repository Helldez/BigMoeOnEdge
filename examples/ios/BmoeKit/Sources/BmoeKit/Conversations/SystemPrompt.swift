import Foundation

/// What `GET /api/conversations/<id>/system` answers (docs/server-api.md): the system prompt the
/// model reads before the conversation, exactly as sent (#625): the agent's lines where it has a
/// folder, then `own`, the person's. `tokens` is what it takes of the context, nil with no model ready
/// or on a provider; `external` when another agent program writes its own.
public struct SystemPrompt: Equatable, Sendable {
    public let text: String
    public let own: String
    public let tokens: Int?
    public let external: Bool

    public init(_ j: JSON) {
        text = j["text"].string(or: "")
        own = j["own"].string(or: "")
        tokens = j["tokens"].int
        external = j["external"].bool ?? false
    }

    /// What the server puts before the person's own text; "" where it adds nothing.
    public var added: String { text == own ? "" : text }
}

extension APIClient {
    public func systemPrompt(conversation id: String) async throws -> SystemPrompt {
        SystemPrompt(try await get("/api/conversations/\(Self.escape(id))/system", timeout: Self.quick))
    }
}
