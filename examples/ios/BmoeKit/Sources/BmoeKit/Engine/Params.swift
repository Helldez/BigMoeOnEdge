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

    /// A value that means something other than its number (`0` is off), as the value's text.
    public struct Special: Equatable, Sendable, Hashable {
        public let value: String
        public let label: String
    }

    public let key: String
    public let label: String
    public let kind: Kind
    public let group: String
    public let groupLabel: String
    /// The group's place in the form (`group_order`); nil from a server that does not send it.
    public let groupOrder: Int?
    /// The group's one line, under its title.
    public let groupHelp: String
    /// The group is set with a control of its own (the model, from Models): shown, not edited.
    public let groupReadOnly: Bool
    public let level: Level
    /// The table offers the row on a settings form (`form`): the short list every front-end shows.
    public let form: Bool
    public let scope: Scope
    public let help: String
    public let flag: String
    public let min: Double?
    public let max: Double?
    public let unit: String?
    /// What a person usually sets: guidance, not a limit.
    public let typical: ClosedRange<Double>?
    public let special: [Special]
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
        groupOrder = j["group_order"].int
        groupHelp = j["group_help"].string(or: "")
        groupReadOnly = j["group_read_only"].bool(or: false)
        // No level is a basic row; a level this app does not know yet is taken as the deepest, so
        // a new kind of row is left off the form rather than shown with the recommended ones.
        level = Level(rawValue: j["level"].string(or: "basic")) ?? .debug
        // A server from before `form` offered its basic rows first: those are the form there.
        form = j["form"].bool(or: level == .basic)
        scope = Scope(rawValue: j["scope"].string(or: "session")) ?? .session
        help = j["help"].string(or: "")
        flag = j["flag"].string(or: "")
        min = j["min"].double
        max = j["max"].double
        let u = j["unit"].string(or: "")
        unit = u.isEmpty ? nil : u
        if let lo = j["typical"]["min"].double, let hi = j["typical"]["max"].double, lo <= hi {
            typical = lo...hi
        } else {
            typical = nil
        }
        special = (j["special"].array ?? []).compactMap {
            let l = $0["label"].string(or: "")
            return $0["value"].isNull || l.isEmpty ? nil : Special(value: $0["value"].display, label: l)
        }
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

    /// Groups in the table's group order (`group_order`, else where a group first appears), each
    /// with its rows in the table's order.
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
        // Sorted on (order, first appearance): Swift's sort is not stable, so the position breaks ties.
        let sorted = order.enumerated().sorted { a, b in
            let oa = by[a.element]!.first!.groupOrder ?? Int.max
            let ob = by[b.element]!.first!.groupOrder ?? Int.max
            return oa != ob ? oa < ob : a.offset < b.offset
        }.map(\.element)
        return sorted.map { (by[$0]!.first!.groupLabel, by[$0]!) }
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

    /// The limits as a person reads them, "1 to 64 threads" or "at least 0 MiB", or nil without
    /// any. Shown only beside a refused value: the typical range is the guide.
    public var rangeText: String? {
        guard let min else { return nil }
        let fmt: (Double) -> String = { JSON.double($0).display }
        let u = unit.map { " \($0)" } ?? ""
        // An older server sends the type's largest value as the upper limit, which says nothing to a person.
        guard let max, max < Double(Int32.max) else { return "at least \(fmt(min))\(u)" }
        return "\(fmt(min)) to \(fmt(max))\(u)"
    }

    /// The name of `value` when it is a special one ("off" for 0), whatever JSON type carries it.
    public func specialLabel(_ value: JSON) -> String? {
        let text = value.display
        return special.first { $0.value == text }?.label
    }

    /// The typical range in words, "usually 2 to 8" (the unit sits by the field), or nil without one.
    public var typicalText: String? {
        guard let typical else { return nil }
        let lo = JSON.double(typical.lowerBound).display, hi = JSON.double(typical.upperBound).display
        return "usually " + (lo == hi ? lo : "\(lo) to \(hi)")
    }

    /// The help's first sentence, always shown, and the rest, shown on demand. The web UI's rule: a
    /// sentence ends at a stop followed by a space and a capital or a digit, outside parentheses
    /// (where "e.g. HTP0" lives), once the lead is long enough to stand alone.
    public var helpParts: (lead: String, rest: String) {
        let text = Array(help.trimmingCharacters(in: .whitespacesAndNewlines))
        var depth = 0
        for (i, c) in text.enumerated() {
            switch c {
            case "(": depth += 1
            case ")": depth -= 1
            case ".", "!", "?":
                let next = i + 1
                guard depth <= 0, next >= 24, next < text.count, text[next].isWhitespace else { continue }
                guard let start = text[next...].firstIndex(where: { !$0.isWhitespace }),
                      text[start].isUppercase || text[start].isNumber else { continue }
                return (String(text[..<next]), String(text[start...]))
            default: break
            }
        }
        return (String(text), "")
    }
}
