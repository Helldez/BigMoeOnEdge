import Foundation

// The agent's shapes as docs/agent.md states them. Read from the JSON tree tolerantly: a field the
// server leaves out reads as its default, so a server a little older or newer than this app still
// draws a conversation instead of failing to decode it.

/// A time as the server may send it: epoch seconds, epoch milliseconds, or an ISO 8601 string (with
/// or without fractional seconds, or a local `YYYY-MM-DDTHH:MM` without a zone).
public enum AgentTime {
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

/// What the agent may do without asking (docs/agent.md, "Mode").
public enum AgentMode: String, CaseIterable, Sendable, Identifiable {
    case ask
    case readOnly = "read_only"
    case full

    public var id: String { rawValue }

    public var label: String {
        switch self {
        case .ask: return "Ask first"
        case .readOnly: return "Read only"
        case .full: return "Full access"
        }
    }

    public var help: String {
        switch self {
        case .ask: return "Reads freely; asks before it changes a file."
        case .readOnly: return "Reads only; never changes a file."
        case .full: return "Reads and changes files without asking."
        }
    }

    public init(wire: String) { self = AgentMode(rawValue: wire) ?? .ask }
}

/// One entry of a conversation, upserted by `id`. One struct over the five kinds: each kind reads
/// the fields its row of the contract names, and the others stay empty.
public struct AgentItem: Equatable, Sendable, Identifiable {
    public enum Kind: String, Sendable { case user, assistant, tool, helper, notice, other }

    public var id: String
    public var kind: Kind
    /// user, assistant, notice
    public var text: String
    /// assistant
    public var reasoning: String
    public var streaming: Bool
    /// tool
    public var tool: String
    /// tool, helper: plain words ("Read src/main.cpp")
    public var title: String
    public var args: JSON
    /// tool, helper: `running`, `waiting`, `done`, `denied`, `error`
    public var state: String
    public var result: String
    public var diff: String
    public var file: String
    /// helper
    public var summary: String
    public var conversation: String
    /// notice: `info`, `warn`, `error`
    public var level: String

    public init(id: String, kind: Kind, text: String = "", reasoning: String = "", streaming: Bool = false,
                tool: String = "", title: String = "", args: JSON = .null, state: String = "", result: String = "",
                diff: String = "", file: String = "", summary: String = "", conversation: String = "", level: String = "")
    {
        self.id = id
        self.kind = kind
        self.text = text
        self.reasoning = reasoning
        self.streaming = streaming
        self.tool = tool
        self.title = title
        self.args = args
        self.state = state
        self.result = result
        self.diff = diff
        self.file = file
        self.summary = summary
        self.conversation = conversation
        self.level = level
    }

    public init(_ j: JSON) {
        self.init(
            id: j["id"].string ?? j["id"].int64.map { String($0) } ?? "",
            kind: Kind(rawValue: j["kind"].string(or: "")) ?? .other,
            text: j["text"].string(or: ""), reasoning: j["reasoning"].string(or: ""),
            streaming: j["streaming"].bool(or: false), tool: j["tool"].string(or: ""), title: j["title"].string(or: ""),
            args: j["args"], state: j["state"].string(or: ""), result: j["result"].string(or: ""),
            diff: j["diff"].string(or: ""), file: j["file"].string(or: ""), summary: j["summary"].string(or: ""),
            conversation: j["conversation"].string(or: ""), level: j["level"].string(or: ""))
    }

    /// A tool waiting for the person: the approval card.
    public var isWaiting: Bool { kind == .tool && state == "waiting" }
}

/// A row of `GET /api/conversations`, and what a `conversation` event changes of it.
public struct ConversationSummary: Equatable, Sendable, Identifiable {
    public var id: String
    public var title: String
    /// The workspace's id; empty for none.
    public var workspace: String
    public var mode: AgentMode
    /// `local` or `<provider id>/<model>`.
    public var route: String
    /// `idle`, `running`, `waiting` (an approval) or `error`.
    public var state: String
    public var updated: Date?
    /// The task that ran it; empty for a conversation a person started.
    public var task: String

    public init(id: String, title: String = "", workspace: String = "", mode: AgentMode = .ask, route: String = "local",
                state: String = "idle", updated: Date? = nil, task: String = "")
    {
        self.id = id
        self.title = title
        self.workspace = workspace
        self.mode = mode
        self.route = route
        self.state = state
        self.updated = updated
        self.task = task
    }

    public init(_ j: JSON) {
        self.init(
            id: j["id"].string(or: ""), title: j["title"].string(or: ""), workspace: j["workspace"].string(or: ""),
            mode: AgentMode(wire: j["mode"].string(or: "ask")), route: j["route"].string(or: "local"),
            state: j["state"].string(or: "idle"), updated: AgentTime.parse(j["updated"]), task: j["task"].string(or: ""))
    }

    public static func list(_ j: JSON) -> [ConversationSummary] { (j.array ?? j["conversations"].array ?? []).map(Self.init) }

    public var busy: Bool { state == "running" || state == "waiting" }
    public var displayTitle: String { title.isEmpty ? "New chat" : title }
}

/// `GET /api/conversations/<id>`: the row's fields and the items.
public struct AgentConversation: Equatable, Sendable {
    public var summary: ConversationSummary
    public var items: [AgentItem]

    public init(summary: ConversationSummary, items: [AgentItem] = []) {
        self.summary = summary
        self.items = items
    }

    public init(_ j: JSON) {
        summary = ConversationSummary(j)
        items = (j["items"].array ?? []).map(AgentItem.init)
    }

    /// Whether the answer carried the items at all: an answer without the key is a server that
    /// did not take them (what the migration checks before it deletes a local copy).
    public static func itemCount(_ j: JSON) -> Int? { j["items"].array?.count }
}

/// A folder a person added: file tools resolve every path inside it.
public struct Workspace: Equatable, Sendable, Identifiable, Hashable {
    public var id: String
    public var name: String
    public var path: String

    public init(id: String, name: String, path: String) {
        self.id = id
        self.name = name
        self.path = path
    }

    public init(_ j: JSON) { self.init(id: j["id"].string(or: ""), name: j["name"].string(or: ""), path: j["path"].string(or: "")) }
    public static func list(_ j: JSON) -> [Workspace] { (j.array ?? []).map(Self.init) }
}

/// A skill: a folder with a `SKILL.md`, from the release, the person or a workspace.
public struct Skill: Equatable, Sendable, Identifiable {
    public var name: String
    public var title: String
    public var description: String
    /// `bundled`, `user` or `workspace`; only a `user` skill is the person's to edit or delete.
    public var source: String
    public var enabled: Bool
    public var body: String

    public var id: String { source + "/" + name }
    public var editable: Bool { source == "user" }

    public init(name: String, title: String = "", description: String = "", source: String = "user", enabled: Bool = true,
                body: String = "")
    {
        self.name = name
        self.title = title
        self.description = description
        self.source = source
        self.enabled = enabled
        self.body = body
    }

    public init(_ j: JSON) {
        self.init(name: j["name"].string(or: ""), title: j["title"].string(or: ""), description: j["description"].string(or: ""),
                  source: j["source"].string(or: "user"), enabled: j["enabled"].bool(or: true), body: j["body"].string(or: ""))
    }

    public static func list(_ j: JSON) -> [Skill] { (j.array ?? []).map(Self.init) }

    /// A skill's name is kebab-case: lower-case letters and digits in words joined by a hyphen.
    public static func validName(_ s: String) -> Bool {
        guard !s.isEmpty, !s.hasPrefix("-"), !s.hasSuffix("-"), !s.contains("--") else { return false }
        return s.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "-" }
    }
}

/// When a task runs (docs/agent.md, "Tasks").
public struct TaskSchedule: Equatable, Sendable {
    public enum Kind: String, CaseIterable, Sendable, Identifiable {
        case once, interval, daily, weekly
        public var id: String { rawValue }
        public var label: String {
            switch self {
            case .once: return "Once"
            case .interval: return "Every few minutes or hours"
            case .daily: return "Every day"
            case .weekly: return "Some days of the week"
            }
        }
    }

    public var kind: Kind
    /// `YYYY-MM-DDTHH:MM` (local) for `once`, `HH:MM` for `daily` and `weekly`.
    public var at: String
    public var everyMinutes: Int
    /// 0 = Sunday … 6 = Saturday.
    public var weekdays: [Int]

    public init(kind: Kind = .daily, at: String = "09:00", everyMinutes: Int = 60, weekdays: [Int] = []) {
        self.kind = kind
        self.at = at
        self.everyMinutes = everyMinutes
        self.weekdays = weekdays
    }

    public init(_ j: JSON) {
        self.init(kind: Kind(rawValue: j["kind"].string(or: "daily")) ?? .daily, at: j["at"].string(or: ""),
                  everyMinutes: j["every_minutes"].int(or: 0), weekdays: (j["weekdays"].array ?? []).compactMap(\.int))
    }

    public var json: JSON {
        ["kind": .string(kind.rawValue), "at": .string(at), "every_minutes": .int(Int64(everyMinutes)),
         "weekdays": .array(weekdays.sorted().map { .int(Int64($0)) })]
    }
}

/// A prompt that runs as a conversation on a schedule.
public struct AgentTask: Equatable, Sendable, Identifiable {
    public struct LastRun: Equatable, Sendable {
        public var at: Date?
        public var conversation: String
        public var outcome: String
    }

    public var id: String
    public var title: String
    public var prompt: String
    public var workspace: String
    public var mode: AgentMode
    public var route: String
    public var schedule: TaskSchedule
    public var enabled: Bool
    public var lastRun: LastRun?
    public var nextRun: Date?

    /// A new task: read only unless the person picks another mode (docs/agent.md).
    public init(id: String = "", title: String = "", prompt: String = "", workspace: String = "", mode: AgentMode = .readOnly,
                route: String = "local", schedule: TaskSchedule = TaskSchedule(), enabled: Bool = true,
                lastRun: LastRun? = nil, nextRun: Date? = nil)
    {
        self.id = id
        self.title = title
        self.prompt = prompt
        self.workspace = workspace
        self.mode = mode
        self.route = route
        self.schedule = schedule
        self.enabled = enabled
        self.lastRun = lastRun
        self.nextRun = nextRun
    }

    public init(_ j: JSON) {
        let last = j["last_run"]
        self.init(
            id: j["id"].string(or: ""), title: j["title"].string(or: ""), prompt: j["prompt"].string(or: ""),
            workspace: j["workspace"].string(or: ""), mode: AgentMode(rawValue: j["mode"].string(or: "")) ?? .readOnly,
            route: j["route"].string(or: "local"), schedule: TaskSchedule(j["schedule"]), enabled: j["enabled"].bool(or: true),
            lastRun: last.object == nil ? nil
                : LastRun(at: AgentTime.parse(last["at"]), conversation: last["conversation"].string(or: ""),
                          outcome: last["outcome"].string(or: "")),
            nextRun: AgentTime.parse(j["next_run"]))
    }

    public static func list(_ j: JSON) -> [AgentTask] { (j.array ?? []).map(Self.init) }

    /// What `POST /api/tasks` and `PUT /api/tasks/<id>` take.
    public var body: JSON {
        ["title": .string(title), "prompt": .string(prompt), "workspace": workspace.isEmpty ? .null : .string(workspace),
         "mode": .string(mode.rawValue), "route": .string(route), "schedule": schedule.json, "enabled": .bool(enabled)]
    }
}

/// A saved connection to an OpenAI-compatible chat service. The key is written, never read back.
public struct Provider: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var baseURL: String
    public var models: [String]
    public var hasKey: Bool

    public init(id: String = "", name: String = "", baseURL: String = "", models: [String] = [], hasKey: Bool = false) {
        self.id = id
        self.name = name
        self.baseURL = baseURL
        self.models = models
        self.hasKey = hasKey
    }

    public init(_ j: JSON) {
        self.init(id: j["id"].string(or: ""), name: j["name"].string(or: ""), baseURL: j["base_url"].string(or: ""),
                  models: j["models"].strings, hasKey: j["has_key"].bool(or: false))
    }

    public static func list(_ j: JSON) -> [Provider] { (j.array ?? []).map(Self.init) }

    /// The routes this provider offers a conversation: `<provider id>/<model>`.
    public var routes: [(route: String, label: String)] { models.map { ("\(id)/\($0)", "\(name) · \($0)") } }
}

/// A route's words for a menu: the loaded model is "This phone", a provider's is its name and model.
public enum AgentRoute {
    public static let local = "local"

    public static func label(_ route: String, providers: [Provider]) -> String {
        if route.isEmpty || route == local { return "This phone" }
        for p in providers { for r in p.routes where r.route == route { return r.label } }
        return route
    }
}
