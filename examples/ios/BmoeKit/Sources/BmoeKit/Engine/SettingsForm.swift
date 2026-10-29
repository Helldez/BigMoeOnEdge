import Foundation

/// The settings form (docs/screens.md, "Settings"): one card per group of the parameter table,
/// holding the rows the table offers on a form (`form`) and any row the person set, and where each
/// value came from. All of it from `GET /api/params` and `GET /api/config`; no group or
/// parameter is named here. The web UI's `lib/schema.ts`.
public enum SettingsForm {
    /// One settings card: a group and its shown rows, in table order.
    public struct Card: Equatable, Sendable, Identifiable {
        public let group: String
        public let label: String
        public let help: String
        /// Shown, not edited: a front-end sets it with a control of its own.
        public let readOnly: Bool
        public var rows: [ParamSpec] = []
        public var id: String { group }
    }

    /// The cards for the table, in its group order (`group_order`, else where a group first
    /// appears), rows in table order. A row is shown when the table offers it on a form or its key
    /// is in `also` (a value the person set, so it can be seen and reset; a row a link points at).
    /// A card left with no row is dropped.
    public static func cards(_ params: [ParamSpec], also: Set<String> = []) -> [Card] {
        var order: [String] = []
        var cards: [String: Card] = [:]
        var position: [String: (order: Int, first: Int)] = [:]
        for (i, p) in params.enumerated() {
            if cards[p.group] == nil {
                order.append(p.group)
                cards[p.group] = Card(group: p.group, label: p.groupLabel, help: p.groupHelp, readOnly: p.groupReadOnly)
                position[p.group] = (p.groupOrder ?? Int.max, i)
            }
            guard p.form || also.contains(p.key) else { continue }
            cards[p.group]!.rows.append(p)
        }
        return order
            .filter { !cards[$0]!.rows.isEmpty }
            .sorted { a, b in
                let pa = position[a]!, pb = position[b]!
                return pa.order != pb.order ? pa.order < pb.order : pa.first < pb.first
            }
            .map { cards[$0]! }
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

    /// Whether there is something to plan or load for, without naming the parameter that holds it:
    /// a row of a read-only group (one a client sets with a control of its own, the model from
    /// Models) holds a value, or a model is loaded. The Android app's `loadTargetChosen`.
    public static func loadTargetChosen(_ params: [ParamSpec], config: ConfigState?, info: ServerInfo?) -> Bool {
        if info?.model != nil { return true }
        guard let config else { return false }
        return params.contains { $0.groupReadOnly && !config[$0.key].display.isEmpty }
    }

    /// The file name of the model a plan is for, without its folder, or nil with none: the plan's
    /// own `model` when one is shown, else the model a load would open (a path in a read-only
    /// group, the model from Models), else the loaded one. The Plan screen's "For <name>".
    public static func planTarget(_ plan: PlanResult?, params: [ParamSpec], config: ConfigState?,
                                  info: ServerInfo?) -> String?
    {
        let chosen = config.flatMap { c in
            params.lazy.filter { $0.groupReadOnly && $0.kind == .path }.map { c[$0.key].display }.first { !$0.isEmpty }
        }
        let loaded = info?.model.map { $0.path.isEmpty ? $0.name : $0.path }
        let shown = plan.flatMap { $0.available && !$0.model.isEmpty ? $0.model : nil }
        guard let path = [shown, chosen, loaded].compactMap({ $0 }).first(where: { !$0.isEmpty }) else { return nil }
        // A plan made on another system may carry its separators: either ends the folder.
        return path.split(whereSeparator: { $0 == "/" || $0 == "\\" }).last.map(String.init) ?? path
    }

    /// The rows shown though the form does not offer them: the ones holding a value the person set.
    public static func userSet(_ params: [ParamSpec], config: ConfigState?) -> Set<String> {
        guard let config else { return [] }
        return Set(params.map(\.key).filter { config.userKeys.contains($0) })
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
