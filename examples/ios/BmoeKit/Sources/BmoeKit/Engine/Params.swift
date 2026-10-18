import Foundation

/// One row of the engine's parameter table, as `GET /api/params` describes it. The settings screen
/// is rendered from these and nothing else: it knows no parameter by name, so a knob added to the
/// engine appears with no app change. The schema the web UI and the Android app render from.
public struct ParamSpec: Equatable, Sendable, Identifiable {
    public enum Kind: String, Sendable { case bool, int, float, choice, path, text }
    public enum Level: String, Sendable, CaseIterable { case basic, advanced, experimental, debug }
    public enum Scope: String, Sendable { case session, request }

    public struct Choice: Equatable, Sendable, Hashable {
        public let value: String
        public let label: String
    }

    public let key: String
    public let label: String
    public let kind: Kind
    public let group: String
    public let groupLabel: String
    public let level: Level
    public let scope: Scope
    public let help: String
    public let flag: String
    public let min: Double?
    public let max: Double?
    public let unit: String?
    public let acceptsAuto: Bool
    /// Changes the reply: the screen marks it.
    public let lossy: Bool
    public let choices: [Choice]
    public let defaultValue: JSON

    public var id: String { key }

    public init(_ j: JSON) {
        key = j["key"].string(or: "")
        label = j["label"].string(or: key)
        kind = Kind(rawValue: j["type"].string(or: "text")) ?? .text
        group = j["group"].string(or: "")
        groupLabel = j["group_label"].string(or: group)
        level = Level(rawValue: j["level"].string(or: "basic")) ?? .basic
        scope = Scope(rawValue: j["scope"].string(or: "session")) ?? .session
        help = j["help"].string(or: "")
        flag = j["flag"].string(or: "")
        min = j["min"].double
        max = j["max"].double
        let u = j["unit"].string(or: "")
        unit = u.isEmpty ? nil : u
        acceptsAuto = j["accepts_auto"].bool(or: false)
        lossy = j["lossy"].bool(or: false)
        choices = (j["choices"].array ?? []).map {
            let v = $0["value"].display
            let l = $0["label"].string(or: "")
            return Choice(value: v, label: l.isEmpty ? v : l)
        }
        defaultValue = j["default"]
    }

    public static func parse(_ j: JSON) -> [ParamSpec] { (j.array ?? []).map(ParamSpec.init) }

    /// Groups in the table's order, each with its rows in the table's order.
    public static func grouped(_ specs: [ParamSpec]) -> [(label: String, rows: [ParamSpec])] {
        var order: [String] = []
        var by: [String: [ParamSpec]] = [:]
        for s in specs {
            if by[s.group] == nil {
                order.append(s.group)
                by[s.group] = []
            }
            by[s.group]!.append(s)
        }
        return order.map { (by[$0]!.first!.groupLabel, by[$0]!) }
    }

    /// What a field's text becomes on the wire: the JSON type the schema says (`bool`, `int`,
    /// `float`) or a string (`choice`, `path`, `text`, and the literal `auto` where the row accepts
    /// it). Nil when the text is not a value of that type: the field shows the problem and nothing
    /// is sent. Bounds are the server's to enforce, and it says so in `rejected`.
    public func value(fromText text: String) -> JSON? {
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if acceptsAuto && t == "auto" { return .string("auto") }
        switch kind {
        case .bool:
            switch t {
            case "true": return .bool(true)
            case "false": return .bool(false)
            default: return nil
            }
        case .int: return Int64(t).map(JSON.int)
        case .float: return Double(t).flatMap { $0.isFinite ? JSON.double($0) : nil }
        case .choice, .path, .text: return .string(t)
        }
    }

    /// The bounds as a person reads them, "1 to 64 threads", or nil without any.
    public var rangeText: String? {
        guard let min, let max else { return nil }
        let fmt: (Double) -> String = { JSON.double($0).display }
        // The table's "no upper bound" is the type's largest value, which says nothing to a person.
        let upper = max >= Double(Int32.max) ? "any" : fmt(max)
        return "\(fmt(min)) to \(upper)" + (unit.map { " \($0)" } ?? "")
    }
}
