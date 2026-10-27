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
    public private(set) var items: [AgentItem]

    public init(conversation: String, items: [AgentItem] = []) {
        self.conversation = conversation
        self.items = items
    }

    /// Applies one event of the stream; answers whether it changed anything.
    @discardableResult
    public mutating func apply(event: String, data: JSON) -> Bool {
        guard data["conversation"].string == conversation else { return false }
        switch event {
        case "item":
            let item = AgentItem(data["item"])
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
                items.append(AgentItem(id: id, kind: .assistant, text: text, reasoning: reasoning, streaming: true))
            }
            return true
        default:
            return false
        }
    }

    public mutating func upsert(_ item: AgentItem) {
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
}

/// The conversation list as `conversation` events change it: a known row takes the new title,
/// state and time; an unknown one is a conversation started elsewhere (a task, another UI) and
/// goes on top, the list being newest first.
public enum ConversationList {
    @discardableResult
    public static func apply(_ list: inout [ConversationSummary], event data: JSON) -> ConversationSummary? {
        guard let id = data["id"].string, !id.isEmpty else { return nil }
        if let i = list.firstIndex(where: { $0.id == id }) {
            if let t = data["title"].string { list[i].title = t }
            if let s = data["state"].string { list[i].state = s }
            if let u = AgentTime.parse(data["updated"]) { list[i].updated = u }
            return list[i]
        }
        let row = ConversationSummary(data)
        list.insert(row, at: 0)
        return row
    }
}

/// How a transcript is drawn: an item on its own, or a run of tool calls folded under one line.
public enum TranscriptRow: Equatable, Identifiable, Sendable {
    case item(AgentItem)
    case group(id: String, summary: String, items: [AgentItem])

    public var id: String {
        switch self {
        case let .item(i): return i.id
        case let .group(id, _, _): return "group-" + id
        }
    }
}

/// The grouped run summary: what a burst of tool calls did, in one line ("Read 3 files, edited 2
/// files"), so a transcript reads as what happened rather than as a log.
public enum RunSummary {
    /// Two or more tool items in a row fold under their summary. A tool waiting for an approval is
    /// never folded: it is the card the person has to answer, and it breaks the run around it.
    public static func rows(_ items: [AgentItem]) -> [TranscriptRow] {
        var out: [TranscriptRow] = []
        var run: [AgentItem] = []
        func flush() {
            if run.count >= 2 {
                out.append(.group(id: run[0].id, summary: summary(run), items: run))
            } else {
                out += run.map(TranscriptRow.item)
            }
            run = []
        }
        for item in items {
            if item.kind == .tool && !item.isWaiting {
                run.append(item)
            } else {
                flush()
                out.append(.item(item))
            }
        }
        flush()
        return out
    }

    /// Counts per kind of tool, in the order each kind first appears, joined by commas, the first
    /// letter capitalised.
    public static func summary(_ tools: [AgentItem]) -> String {
        var order: [String] = []
        var counts: [String: Int] = [:]
        for t in tools {
            let key = phraseKey(t.tool)
            if counts[key] == nil { order.append(key) }
            counts[key, default: 0] += 1
        }
        let text = order.map { phrase($0, counts[$0] ?? 0) }.joined(separator: ", ")
        return text.prefix(1).uppercased() + text.dropFirst()
    }

    private static let known = ["read_file", "list_files", "search_files", "write_file", "edit_file", "fetch_url",
                                "web_search", "use_skill", "run_command"]

    private static func phraseKey(_ tool: String) -> String { known.contains(tool) ? tool : "other" }

    private static func phrase(_ key: String, _ n: Int) -> String {
        func noun(_ one: String, _ many: String) -> String { "\(n) \(n == 1 ? one : many)" }
        switch key {
        case "read_file": return "read " + noun("file", "files")
        case "list_files": return "listed " + noun("folder", "folders")
        case "search_files": return "searched " + noun("time", "times")
        case "write_file": return "wrote " + noun("file", "files")
        case "edit_file": return "edited " + noun("file", "files")
        case "fetch_url": return "fetched " + noun("page", "pages")
        case "web_search": return "searched the web " + noun("time", "times")
        case "use_skill": return "used " + noun("skill", "skills")
        case "run_command": return "ran " + noun("command", "commands")
        default: return "used " + noun("tool", "tools")
        }
    }
}
