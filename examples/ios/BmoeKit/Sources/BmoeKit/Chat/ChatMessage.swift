import Foundation

/// One option of a decision and the probability the model put on it.
public struct ChoiceScore: Equatable, Sendable, Codable, Hashable {
    public let label: String
    public let text: String
    public let probability: Double
}

/// A turn that failed: the server's message for a person and its code for the app to decide on.
public struct TurnError: Equatable, Sendable, Error {
    public let message: String
    public let code: String

    public init(message: String, code: String) {
        self.message = message
        self.code = code
    }
}

/// One message of a conversation as this app keeps it. `reasoning` is the model's thinking span
/// (shown folded above the answer, and sent back as `reasoning_content` so a reasoning model sees
/// its own trace); `metrics` is the one-line figure under the answer. Neither is engine state: the
/// server keeps no conversation.
public struct ChatMessage: Equatable, Sendable, Codable, Identifiable {
    public enum Role: String, Sendable, Codable { case system, user, assistant }

    public var id: UUID
    public var role: Role
    public var content: String
    public var reasoning: String
    public var metrics: String
    /// A decision's answer: every option with its probability, `best` the one the model picked.
    public var choices: [ChoiceScore]?
    public var best: Int

    public init(role: Role, content: String, reasoning: String = "", metrics: String = "", choices: [ChoiceScore]? = nil,
                best: Int = -1)
    {
        id = UUID()
        self.role = role
        self.content = content
        self.reasoning = reasoning
        self.metrics = metrics
        self.choices = choices
        self.best = best
    }

    public var isDecision: Bool { choices != nil }
}
