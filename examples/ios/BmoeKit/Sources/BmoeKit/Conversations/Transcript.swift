import Foundation

/// The open conversation's items as the events build them (docs/agent.md, "Events"). Pure: the
/// screen feeds it the `item` and `delta` events of the stream and draws what it holds.
///
/// - `item` carries an item whole: it replaces the one with its id where it stands, or is appended,
///   so the order is the order items first appeared in.
/// - `delta` carries what a streaming assistant item adds; one whose `item` event has not arrived
///   yet starts it, streaming.
/// - Events of another conversation change nothing here.
public struct Transcript: Equatable, Sendable {
    public var conversation: String
    public private(set) var items: [ConversationItem]

    public init(conversation: String, items: [ConversationItem] = []) {
        self.conversation = conversation
        self.items = items
    }

    /// Applies one event of the stream; answers whether it changed anything.
    @discardableResult
    public mutating func apply(event: String, data: JSON) -> Bool {
        guard data["conversation"].string == conversation else { return false }
        switch event {
        case "item":
            let item = ConversationItem(data["item"])
            guard !item.id.isEmpty else { return false }
            upsert(item)
            return true
        case "delta":
            let id = data["item"].string ?? data["item"].int64.map { String($0) } ?? ""
            guard !id.isEmpty else { return false }
            let text = data["text"].string ?? ""
            let reasoning = data["reasoning"].string ?? ""
            if let i = items.firstIndex(where: { $0.id == id }) {
                items[i].text += text
                items[i].reasoning += reasoning
            } else {
                items.append(ConversationItem(id: id, kind: .assistant, text: text, reasoning: reasoning, streaming: true))
            }
            return true
        default:
            return false
        }
    }

    public mutating func upsert(_ item: ConversationItem) {
        if let i = items.firstIndex(where: { $0.id == item.id }) {
            items[i] = item
        } else {
            items.append(item)
        }
    }

    /// What edit-and-resend leaves: the item and everything after it are dropped by the server; the
    /// screen drops them at once rather than show them until the events catch up.
    public mutating func truncate(from id: String) {
        if let i = items.firstIndex(where: { $0.id == id }) { items.removeSubrange(i...) }
    }

    /// The person's last message: what Answer again resends, through the edit route, as it was
    /// (as the web UI does).
    public var lastUser: ConversationItem? { items.last { $0.kind == .user } }

    /// The item that carries Answer again: the last answer, once it is whole, when nothing but
    /// notices follows it (a newer question has its own answer coming).
    public var answerAgainOn: String? {
        guard let last = items.last(where: { $0.kind != .notice }), last.kind == .assistant, !last.streaming,
              lastUser != nil
        else { return nil }
        return last.id
    }

    /// The body of `POST /api/conversations/<id>/edit`: the message `item` replaced by `text`.
    public static func editBody(item: String, text: String) -> JSON {
        ["item": .string(item), "text": .string(text)]
    }
}

/// The conversation list as `conversation` events change it: a known row takes the new title,
/// state and time; an unknown one is a conversation started elsewhere (another client of this
/// server) and goes on top, the list being newest first.
public enum ConversationList {
    @discardableResult
    public static func apply(_ list: inout [ConversationSummary], event data: JSON) -> ConversationSummary? {
        guard let id = data["id"].string, !id.isEmpty else { return nil }
        if let i = list.firstIndex(where: { $0.id == id }) {
            if let t = data["title"].string { list[i].title = t }
            if let s = data["state"].string { list[i].state = s }
            if let u = ServerTime.parse(data["updated"]) { list[i].updated = u }
            return list[i]
        }
        let row = ConversationSummary(data)
        list.insert(row, at: 0)
        return row
    }
}
