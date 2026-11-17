import Foundation

/// What `GET /api/conversations/<id>/system` answers (docs/server-api.md): the system prompt the
/// server sends before the conversation, exactly as sent, read-only (#625). `tokens` is what it takes
/// of the context, nil with no model ready or on a provider; `external` when another agent program
/// writes its own.
public struct SystemPrompt: Equatable, Sendable {
    public let text: String
    public let tokens: Int?
    public let external: Bool

    public init(_ j: JSON) {
        text = j["text"].string(or: "")
        tokens = j["tokens"].int
        external = j["external"].bool ?? false
    }
}

extension APIClient {
    public func systemPrompt(conversation id: String) async throws -> SystemPrompt {
        SystemPrompt(try await get("/api/conversations/\(Self.escape(id))/system", timeout: Self.quick))
    }
}
