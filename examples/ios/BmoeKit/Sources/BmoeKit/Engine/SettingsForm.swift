import Foundation

/// The settings form (docs/screens.md, "Settings"): one card per group of the parameter table, its
/// basic rows open and the rest folded by level, a level filter, a search, and where each value
/// came from. All of it from `GET /api/params` and `GET /api/config`; no group or parameter is
/// named here. The web UI's `lib/schema.ts`.
public enum SettingsForm {
    /// The rows of one level that a card folds away.
    public struct LevelRows: Equatable, Sendable {
        public let level: ParamSpec.Level
        public var rows: [ParamSpec]
    }

    /// One settings card: a group, its basic rows, the deeper rows by level, what the filter hides.
    public struct Card: Equatable, Sendable, Identifiable {
        public let group: String
        public let label: String
        public let help: String
        /// Shown, not edited: a front-end sets it with a control of its own.
        public let readOnly: Bool
        public var basic: [ParamSpec] = []
        public var more: [LevelRows] = []
        /// Rows of this group the level filter leaves out.
        public var hidden = 0
        public var id: String { group }

        /// How many rows its folds hold.
        public var moreCount: Int { more.reduce(0) { $0 + $1.rows.count } }
    }

    /// The cards for the table, in its group order (`group_order`, else where a group first
    /// appears), rows in table order. With `query`, every level is searched and only matching rows
    /// are kept; otherwise rows deeper than `upTo` are hidden, except the `pinned` ones (a value
    /// someone or the plan set is never hidden). A card left with no row is dropped.
    public static func cards(_ params: [ParamSpec], upTo: ParamSpec.Level, query: String = "",
                             pinned: Set<String> = []) -> [Card]
    {
        let words = query.lowercased().split(whereSeparator: \.isWhitespace).map(String.init)
        let depth = rank(upTo)
        var order: [String] = []
        var cards: [String: Card] = [:]
        var position: [String: (order: Int, first: Int)] = [:]
        for (i, p) in params.enumerated() {
            if cards[p.group] == nil {
                order.append(p.group)
                cards[p.group] = Card(group: p.group, label: p.groupLabel, help: p.groupHelp, readOnly: p.groupReadOnly)
                position[p.group] = (p.groupOrder ?? Int.max, i)
            }
            let shown = words.isEmpty
                ? p.level == .basic || rank(p.level) <= depth || pinned.contains(p.key)
                : matches(p, words)
            guard shown else {
                if words.isEmpty { cards[p.group]!.hidden += 1 }
                continue
            }
            if p.level == .basic {
                cards[p.group]!.basic.append(p)
            } else if let k = cards[p.group]!.more.firstIndex(where: { $0.level == p.level }) {
                cards[p.group]!.more[k].rows.append(p)
            } else {
                cards[p.group]!.more.append(LevelRows(level: p.level, rows: [p]))
            }
        }
        return order
            .filter { !cards[$0]!.basic.isEmpty || !cards[$0]!.more.isEmpty }
            .sorted { a, b in
                let pa = position[a]!, pb = position[b]!
                return pa.order != pb.order ? pa.order < pb.order : pa.first < pb.first
            }
            .map { g in
                var c = cards[g]!
                c.more.sort { rank($0.level) < rank($1.level) }
                return c
            }
    }

    static func rank(_ l: ParamSpec.Level) -> Int { ParamSpec.Level.allCases.firstIndex(of: l) ?? 0 }

    private static func matches(_ p: ParamSpec, _ words: [String]) -> Bool {
        let hay = "\(p.label) \(p.help) \(p.flag) \(p.key) \(p.groupLabel)".lowercased()
        return words.allSatisfy { hay.contains($0) }
    }

    /// How many rows each level holds, for the filter's counts.
    public static func levelCounts(_ params: [ParamSpec]) -> [ParamSpec.Level: Int] {
        params.reduce(into: [:]) { $0[$1.level, default: 0] += 1 }
    }

    /// The filter that shows `level`: `current` when it already does, else widened to it.
    public static func widened(_ current: ParamSpec.Level, toShow level: ParamSpec.Level) -> ParamSpec.Level {
        rank(level) > rank(current) ? level : current
    }

    /// What "Reset all my values" removes: the person's own values, never a key the server was
    /// launched with (the operator's choice for this run) and never a row of a read-only group,
    /// which a client sets with a control of its own (the model a load chose).
    public static func resettable(_ config: ConfigState, params: [ParamSpec]) -> [String] {
        let readOnly = Set(params.filter(\.groupReadOnly).map(\.key))
        // In table order, so the count and the request read the same everywhere.
        let known = params.map(\.key).filter { config.userKeys.contains($0) }
        let rest = config.userKeys.subtracting(known).sorted()
        return (known + rest).filter { !config.launchKeys.contains($0) && !readOnly.contains($0) }
    }

    /// The rows shown whatever the filter: a value the person set, one a plan decided, one off its default.
    public static func pinned(_ params: [ParamSpec], config: ConfigState?) -> Set<String> {
        guard let config else { return [] }
        let planned = Set((config.plan ?? []).map(\.knob))
        return Set(params.filter {
            config.userKeys.contains($0.key) || planned.contains($0.key) || !isDefault($0, config[$0.key])
        }.map(\.key))
    }

    /// Where a value came from, in the one chip a row carries.
    public enum Source: Equatable, Sendable {
        case you, plan, standard

        /// The chip's words.
        public var text: String {
            switch self {
            case .you: return "set by you"
            case .plan: return "from the plan"
            case .standard: return "default"
            }
        }
    }

    /// The person's value wins; else a plan that decided the row; else the server's default.
    public static func source(userSet: Bool, decision: PlanDecision?) -> Source {
        if userSet { return .you }
        if let decision, decision.isSet { return .plan }
        return .standard
    }

    /// The plan decided this row and the value in force is not the one it decided: the row offers
    /// "Use plan value", which removes the person's value so the plan's applies.
    public static func differsFromPlan(_ value: JSON, _ decision: PlanDecision?) -> Bool {
        guard let decision, decision.isSet, !value.isNull else { return false }
        return !(value.sameValue(as: .string(decision.value)) || value.display == decision.value)
    }

    public static func isDefault(_ spec: ParamSpec, _ value: JSON) -> Bool {
        value.isNull || spec.defaultValue.sameValue(as: value)
    }

    /// What a level means, in a line under its fold. The four levels are the table's enum; the
    /// rows in them are the server's.
    public static func levelHint(_ level: ParamSpec.Level) -> String {
        switch level {
        case .basic: return ""
        case .advanced: return "Measured levers with a known trade-off."
        case .experimental: return "Measured once or on one device: they can slow a run down."
        case .debug: return "Tests, diagnostics and A/B baselines; never a speed setting."
        }
    }
}

extension PlanDecision {
    /// A setting the plan decided (a decision with no group, from an older server, is one).
    public var isSet: Bool { group != "kept" && group != "notes" }
}

extension JSON {
    /// Equality across the JSON types a parameter takes: `8` and `"8"`, `8` and `8.0`, the same.
    public func sameValue(as other: JSON) -> Bool {
        if self == other { return true }
        switch (self, other) {
        case let (.string(s), _) where other.double != nil:
            return Double(s.trimmingCharacters(in: .whitespaces)) == other.double
        case let (_, .string(s)) where double != nil:
            return Double(s.trimmingCharacters(in: .whitespaces)) == double
        default:
            if let a = double, let b = other.double { return a == b }
            return false
        }
    }
}
