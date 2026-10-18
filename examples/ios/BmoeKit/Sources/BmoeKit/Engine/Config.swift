import Foundation

/// One decision of a plan: the knob, its value, where it came from and why.
public struct PlanDecision: Equatable, Sendable, Identifiable {
    public let knob: String
    public let value: String
    /// `measured`, `derived`, `policy`, `operator` or `unprobed`.
    public let source: String
    public let reason: String
    public var id: String { knob }

    public static func list(_ j: JSON) -> [PlanDecision] {
        (j.array ?? []).map {
            PlanDecision(knob: $0["knob"].string(or: ""), value: $0["value"].display, source: $0["source"].string(or: ""),
                         reason: $0["reason"].string(or: ""))
        }
    }
}

/// `GET /api/config`, and every `config` event: the values, and which of them the person set.
public struct ConfigState: Equatable, Sendable {
    public let values: [String: JSON]
    public let userKeys: Set<String>
    /// The equivalent `bmoe-cli` command line, for "copy as command".
    public let args: [String]
    public let valid: Bool
    public let error: String
    public let reloadRequired: Bool
    /// The last applied plan's decisions, or nil when none was applied.
    public let plan: [PlanDecision]?
    public let autoPlan: Bool
    /// Values `PUT /api/config` could not parse, with the reason.
    public let rejected: [String: String]

    public init(_ j: JSON) {
        values = j["values"].object ?? [:]
        userKeys = Set(j["user_keys"].strings)
        args = j["args"].strings
        valid = j["valid"].bool(or: true)
        error = j["error"].string(or: "")
        reloadRequired = j["reload_required"].bool(or: false)
        plan = j["plan"].isNull ? nil : PlanDecision.list(j["plan"])
        autoPlan = j["auto_plan"].bool(or: false)
        rejected = (j["rejected"].object ?? [:]).mapValues { $0.display }
    }

    public subscript(key: String) -> JSON { values[key] ?? .null }
}

/// `GET /api/plan`: what the planner measured and what it decided for the configured model.
public struct PlanResult: Equatable, Sendable {
    public let available: Bool
    public let error: String
    public let model: String
    public let machine: String
    public let regime: String
    public let streamingDeclined: Bool
    public let declineReason: String
    public let decisions: [PlanDecision]
    public let args: [String]
    public let notApplicable: [String]
    public let warning: String
    public let fromLastLoad: Bool
    public let explain: String

    public init(_ j: JSON) {
        available = j["available"].bool(or: false)
        let e = j["error"].string(or: "")
        error = e.isEmpty ? j["reason"].string(or: "") : e
        model = j["model"].string(or: "")
        machine = j["machine"].string(or: "")
        regime = j["regime"].string(or: "")
        streamingDeclined = j["streaming_declined"].bool(or: false)
        declineReason = j["decline_reason"].string(or: "")
        decisions = PlanDecision.list(j["decisions"])
        args = j["args"].strings
        notApplicable = j["not_applicable"].strings
        warning = j["warning"].string(or: "")
        fromLastLoad = j["from_last_load"].bool(or: false)
        explain = j["explain"].string(or: "")
    }

    /// The command line that reproduces the plan, for a bug report.
    public var reproduceLine: String {
        (["bmoe-cli", "-m", model.isEmpty ? "MODEL" : model] + args).map(Shell.quote).joined(separator: " ")
    }
}

/// Quoting for a command line a person copies.
public enum Shell {
    public static func quote(_ s: String) -> String {
        if !s.isEmpty, s.allSatisfy({ $0.isLetter || $0.isNumber || "-_./=:,+@%".contains($0) }) { return s }
        return "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}
