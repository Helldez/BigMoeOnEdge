import Foundation

// A conversation's shapes as docs/agent.md states them. The app starts its server with
// `--chat-only`, so a conversation here is plain chat: the person's messages, the model's answers
// and the server's notices. Read from the JSON tree tolerantly: a field the server leaves out reads
// as its default, so a server a little older or newer than this app still draws a conversation
// instead of failing to decode it.

/// A time as the server may send it: epoch seconds, epoch milliseconds, or an ISO 8601 string (with
/// or without fractional seconds, or a local `YYYY-MM-DDTHH:MM` without a zone).
public enum ServerTime {
    public static func parse(_ j: JSON) -> Date? {
        if let d = j.double {
            guard d > 0 else { return nil }
            return Date(timeIntervalSince1970: d > 1e12 ? d / 1000 : d)
        }
        guard let s = j.string, !s.isEmpty else { return nil }
        return parse(s)
    }

    public static func parse(_ s: String) -> Date? {
        let plain = ISO8601DateFormatter()
        if let d = plain.date(from: s) { return d }
        let fractional = ISO8601DateFormatter()
        fractional.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        if let d = fractional.date(from: s) { return d }
        let local = DateFormatter()
        local.locale = Locale(identifier: "en_US_POSIX")
        for format in ["yyyy-MM-dd'T'HH:mm:ss", "yyyy-MM-dd'T'HH:mm", "yyyy-MM-dd HH:mm"] {
            local.dateFormat = format
            if let d = local.date(from: s) { return d }
        }
        return nil
    }
}

/// One entry of a conversation, upserted by `id`. A kind this app does not draw reads as `other`.
public struct ConversationItem: Equatable, Sendable, Identifiable {
    public enum Kind: String, Sendable { case user, assistant, notice, other }

    public var id: String
    public var kind: Kind
    /// user, assistant, notice
    public var text: String
    /// assistant
    public var reasoning: String
    public var streaming: Bool
    /// notice: `info`, `warn`, `error`
    public var level: String

    public init(id: String, kind: Kind, text: String = "", reasoning: String = "", streaming: Bool = false, level: String = "") {
        self.id = id
        self.kind = kind
        self.text = text
        self.reasoning = reasoning
        self.streaming = streaming
        self.level = level
    }

    public init(_ j: JSON) {
        self.init(
            id: j["id"].string ?? j["id"].int64.map { String($0) } ?? "",
            kind: Kind(rawValue: j["kind"].string(or: "")) ?? .other,
            text: j["text"].string(or: ""), reasoning: j["reasoning"].string(or: ""),
            streaming: j["streaming"].bool(or: false), level: j["level"].string(or: ""))
    }
}

/// A row of `GET /api/conversations`, and what a `conversation` event changes of it.
public struct ConversationSummary: Equatable, Sendable, Identifiable {
    public var id: String
    public var title: String
    /// `idle`, `running` or `error`.
    public var state: String
    public var updated: Date?

    public init(id: String, title: String = "", state: String = "idle", updated: Date? = nil) {
        self.id = id
        self.title = title
        self.state = state
        self.updated = updated
    }

    public init(_ j: JSON) {
        self.init(id: j["id"].string(or: ""), title: j["title"].string(or: ""), state: j["state"].string(or: "idle"),
                  updated: ServerTime.parse(j["updated"]))
    }

    public static func list(_ j: JSON) -> [ConversationSummary] { (j.array ?? j["conversations"].array ?? []).map(Self.init) }

    public var busy: Bool { state == "running" }
    public var displayTitle: String { title.isEmpty ? "New chat" : title }
}

/// `GET /api/conversations/<id>`: the row's fields and the items.
public struct ServerConversation: Equatable, Sendable {
    public var summary: ConversationSummary
    public var items: [ConversationItem]

    public init(summary: ConversationSummary, items: [ConversationItem] = []) {
        self.summary = summary
        self.items = items
    }

    public init(_ j: JSON) {
        summary = ConversationSummary(j)
        items = (j["items"].array ?? []).map(ConversationItem.init)
    }

    /// Whether the answer carried the items at all: an answer without the key is a server that
    /// did not take them (what the migration checks before it deletes a local copy).
    public static func itemCount(_ j: JSON) -> Int? { j["items"].array?.count }
}
