import Foundation

/// One decision of a plan: the knob, its value, where it came from and why, in the server's words.
public struct PlanDecision: Equatable, Sendable {
    public let knob: String
    public let value: String
    /// `measured`, `derived`, `policy`, `operator` or `unprobed`.
    public let source: String
    /// The full account `explain` prints, for a "why" a screen folds away.
    public let reason: String
    /// The knob's settings label, or the name of a note; empty from an older server.
    public let label: String
    /// `set` (decided from a fact), `kept` (left as it was) or `notes` (no setting).
    public let group: String
    /// The decision in one short line; empty where the server has none (the reason is shown).
    public let brief: String
    /// `value` as a person reads it ("4.6 GiB", "pinned in memory").
    public let valueText: String

    public init(knob: String, value: String, source: String, reason: String, label: String = "", group: String = "",
                brief: String = "", valueText: String = "")
    {
        self.knob = knob
        self.value = value
        self.source = source
        self.reason = reason
        self.label = label
        self.group = group
        self.brief = brief
        self.valueText = valueText
    }

    public static func list(_ j: JSON) -> [PlanDecision] {
        (j.array ?? []).map {
            PlanDecision(knob: $0["knob"].string(or: ""), value: $0["value"].display, source: $0["source"].string(or: ""),
                         reason: $0["reason"].string(or: ""), label: $0["label"].string(or: ""),
                         group: $0["group"].string(or: ""), brief: $0["brief"].string(or: ""),
                         valueText: $0["value_text"].string(or: ""))
        }
    }

    /// The decisions in the three groups a plan is read in, in the server's order; a decision with
    /// no group (an older server) is one the plan set.
    public static func grouped(_ all: [PlanDecision]) -> (set: [PlanDecision], kept: [PlanDecision], notes: [PlanDecision]) {
        (all.filter { $0.group != "kept" && $0.group != "notes" }, all.filter { $0.group == "kept" },
         all.filter { $0.group == "notes" })
    }

    /// What a row names: the label, else the knob.
    public var title: String { label.isEmpty ? knob : label }

    /// The value as a row shows it: the words, else the raw value unless it is the placeholder `-`.
    public var shownValue: String { !valueText.isEmpty ? valueText : (value == "-" ? "" : value) }
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
    /// The parameters whose value differs from the loaded session's, in table order: the rows
    /// marked "needs reload". Empty with no session.
    public let reloadKeys: [String]
    /// The person's keys the server was launched with, which "Reset all my values" leaves alone.
    public let launchKeys: Set<String>
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
        reloadKeys = j["reload_keys"].strings
        launchKeys = Set(j["launch_keys"].strings)
        plan = j["plan"].isNull ? nil : PlanDecision.list(j["plan"])
        autoPlan = j["auto_plan"].bool(or: false)
        rejected = (j["rejected"].object ?? [:]).mapValues { $0.display }
    }

    public subscript(key: String) -> JSON { values[key] ?? .null }
}

/// `GET /api/plan`: what the planner measured and what it decided for the configured model, and
/// the plan as a screen draws it (`server/plan_view.h`). Every word a screen shows arrives here; a
/// code beside a word (`phase`, `uses`, `status`) is kept only to pick a colour by.
public struct PlanResult: Equatable, Sendable {
    /// Which phase runs where: the lane's words and the device names on it.
    public struct Lane: Equatable, Sendable {
        public let phase: String
        public let label: String
        public let on: [String]
    }

    /// Decode as the plan priced it, or as a finished turn measured it: seconds per token (0 when
    /// absent) and the cache hit rate (-1 when absent or without a cache).
    public struct Decode: Equatable, Sendable {
        public let sPerToken: Double
        public let cacheHitPct: Double
        public let tokens: Int
        /// The same token with nothing held in memory and every byte read again (predicted only; 0
        /// when absent): a bound beside the prediction.
        public let sPerTokenWorst: Double

        init(_ j: JSON) {
            sPerTokenWorst = j["s_per_token_worst"].double(or: 0)
            sPerToken = j["s_per_token"].double(or: 0)
            cacheHitPct = j["cache_hit_pct"].double(or: -1)
            tokens = j["tokens"].int(or: 0)
        }
    }

    /// One line of the memory ledger. `inTurn`: held in the expert cache's place while a prompt runs.
    public struct MemoryRow: Equatable, Sendable {
        public let name: String
        public let label: String
        public let bytes: Int64
        public let locked: Bool
        public let inTurn: Bool
    }

    /// One memory pool: the host's, or a device's own. `estimated`: an even share of a sum.
    public struct Pool: Equatable, Sendable {
        public let name: String
        public let label: String
        public let capBytes: Int64
        public let lockableCapBytes: Int64
        public let heldBytes: Int64
        public let lockedBytes: Int64
        public let estimated: Bool
        public let rows: [MemoryRow]
    }

    /// A device the plan saw: its rate and badge in words, its role as codes, and why.
    public struct Device: Equatable, Sendable {
        public let name: String
        public let description: String
        public let cpu: Bool
        public let promptRatio: Double
        public let tokenRatio: Double
        public let rateText: String
        /// Which of `prompt`, `tokens`, `layers` it does in this run.
        public let uses: [String]
        /// `used`, `unused`, `refused` or `unusable`: a badge's colour, never its words.
        public let status: String
        public let badge: String
        public let why: String
    }

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
    /// The server holds no plan yet (`?cached=1`): nothing was measured.
    public let none: Bool
    /// The last plan the server holds, read without measuring.
    public let cached: Bool
    /// What the plan decided, in one sentence; empty from a server that does not say.
    public let sentence: String
    public let lanes: [Lane]
    public let predicted: Decode?
    /// What the loaded model's last complete turn measured; nil before one.
    public let measured: Decode?
    public let pools: [Pool]
    public let devices: [Device]
    /// Why there is no device beyond the host, in words; empty when there is one.
    public let devicesNoneText: String

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
        none = j["none"].bool(or: false)
        cached = j["cached"].bool(or: false)
        sentence = j["summary"]["sentence"].string(or: "")
        lanes = (j["summary"]["lanes"].array ?? []).map {
            let phase = $0["phase"].string(or: "")
            return Lane(phase: phase, label: $0["label"].string(or: phase), on: $0["on"].strings)
        }
        predicted = j["predicted"].object == nil ? nil : Decode(j["predicted"])
        measured = j["measured"].object == nil ? nil : Decode(j["measured"])
        pools = (j["memory"]["pools"].array ?? []).map { p in
            let name = p["name"].string(or: "")
            return Pool(name: name, label: p["label"].string(or: name), capBytes: p["cap_bytes"].int64(or: 0),
                        lockableCapBytes: p["lockable_cap_bytes"].int64(or: 0), heldBytes: p["held_bytes"].int64(or: 0),
                        lockedBytes: p["locked_bytes"].int64(or: 0), estimated: p["estimated"].bool(or: false),
                        rows: (p["rows"].array ?? []).map { r in
                            let n = r["name"].string(or: "")
                            return MemoryRow(name: n, label: r["label"].string(or: n), bytes: r["bytes"].int64(or: 0),
                                             locked: r["locked"].bool(or: false), inTurn: r["in_turn"].bool(or: false))
                        })
        }
        devices = (j["devices"].array ?? []).map { d in
            let status = d["status"].string(or: "")
            return Device(name: d["name"].string(or: ""), description: d["description"].string(or: ""),
                          cpu: d["cpu"].bool(or: false), promptRatio: d["prompt_ratio"].double(or: 0),
                          tokenRatio: d["token_ratio"].double(or: 0), rateText: d["rate_text"].string(or: ""),
                          uses: d["uses"].strings, status: status, badge: d["badge"].string(or: status),
                          why: d["why"].string(or: ""))
        }
        devicesNoneText = j["devices_none_text"].string(or: "")
    }

    /// The command line that reproduces the plan, for a bug report.
    public var reproduceLine: String {
        (["bmoe-cli", "-m", model.isEmpty ? "MODEL" : model] + args).map(Shell.quote).joined(separator: " ")
    }

    /// Decode speed, expected against measured: tokens per second each (nil when absent) and the
    /// measured one's difference from the expected one, in percent.
    public var speedCheck: (expected: Double?, measured: Double?, deltaPct: Double?) {
        let p = predicted.flatMap { $0.sPerToken > 0 ? 1 / $0.sPerToken : nil }
        let m = measured.flatMap { $0.sPerToken > 0 ? 1 / $0.sPerToken : nil }
        guard let p, let m else { return (p, m, nil) }
        return (p, m, 100 * (m / p - 1))
    }

    /// The four figures under the plan's sentence (docs/screens.md, "Plan"), each nil when the
    /// plan does not have it.
    public struct Figures: Equatable, Sendable {
        /// Decode in tokens per second, expected and measured, and the measured one's difference.
        public let expectedTokS: Double?
        public let measuredTokS: Double?
        public let deltaPct: Double?
        /// The cache hit rate the cost model credited, and the one the last turn measured.
        public let creditedHitPct: Double?
        public let measuredHitPct: Double?
        /// What the host pool holds for the run, against what the process may hold.
        public let heldBytes: Int64?
        public let capBytes: Int64?
        /// Milliseconds per token with nothing held in memory, and the expected one beside it.
        public let worstMs: Double?
        public let expectedMs: Double?
    }

    public var figures: Figures {
        let s = speedCheck
        let host = pools.first { $0.name == "host" } ?? pools.first
        let worst = predicted.map(\.sPerTokenWorst).flatMap { $0 > 0 ? $0 * 1000 : nil }
        return Figures(
            expectedTokS: s.expected, measuredTokS: s.measured, deltaPct: s.deltaPct,
            creditedHitPct: predicted.map(\.cacheHitPct).flatMap { $0 >= 0 ? $0 : nil },
            measuredHitPct: measured.map(\.cacheHitPct).flatMap { $0 >= 0 ? $0 : nil },
            heldBytes: host.flatMap { $0.capBytes > 0 ? $0.heldBytes : nil },
            capBytes: host.flatMap { $0.capBytes > 0 ? $0.capBytes : nil },
            worstMs: worst, expectedMs: s.expected.map { 1000 / $0 })
    }

    /// What "Copy diagnostic" puts on the clipboard, for a bug report: the command that reproduces
    /// the plan and the planner's whole account, which the screen itself does not show.
    public var diagnostic: String {
        let cmd = args.isEmpty ? "(no arguments: the plan declines streaming)" : reproduceLine
        let account = explain.trimmingCharacters(in: .whitespacesAndNewlines)
        return account.isEmpty ? cmd + "\n" : "\(cmd)\n\n\(account)\n"
    }
}

/// One setting a plan's Apply changed, worded from the parameter table, for the list under Apply.
public struct AppliedChange: Equatable, Sendable, Identifiable {
    public let key: String
    /// The row's settings label; the key where the table has no row for it.
    public let label: String
    public let from: String
    public let to: String
    public var id: String { key }

    /// What applying a plan changed, row by row in table order, then any key the table lacks.
    public static func between(_ before: [String: JSON], _ after: [String: JSON], params: [ParamSpec]) -> [AppliedChange] {
        let known = Set(params.map(\.key))
        let keys = params.map(\.key) + after.keys.filter { !known.contains($0) }.sorted()
        return keys.compactMap { key in
            guard let to = after[key] else { return nil }
            let from = before[key] ?? .null
            if from.sameValue(as: to) { return nil }
            let spec = params.first { $0.key == key }
            return AppliedChange(key: key, label: spec.map(\.label) ?? key, from: shown(spec, from), to: shown(spec, to))
        }
    }

    /// A value as the settings form shows it: a special value or a choice by its name, a number
    /// with its unit.
    static func shown(_ spec: ParamSpec?, _ v: JSON) -> String {
        if v.isNull { return "" }
        if let named = spec?.specialLabel(v) { return named }
        if let choice = spec?.choices.first(where: { $0.value == v.display }) { return choice.label }
        if let unit = spec?.unit, v.double != nil { return "\(v.display) \(unit)" }
        return v.display
    }
}

/// A `plan_progress` event: a stage of a measurement starting, with its words. Nil for the closing
/// `{"step": "done"}` (or an empty step): nothing is measuring any more.
public struct PlanProgress: Equatable, Sendable {
    public let step: String
    public let label: String
    public let index: Int
    public let count: Int

    public init?(_ j: JSON) {
        let s = j["step"].string(or: "")
        guard !s.isEmpty, s != "done" else { return nil }
        step = s
        let l = j["label"].string(or: "")
        label = l.isEmpty ? s : l
        index = j["index"].int(or: 0)
        count = j["count"].int(or: 0)
    }

    /// How far the measurement is, from 0 to 1.
    public var fraction: Double { count > 0 ? Double(index) / Double(count) : 0 }
    /// The stages still to run after this one.
    public var left: Int { max(0, count - index - 1) }
}

/// Quoting for a command line a person copies.
public enum Shell {
    public static func quote(_ s: String) -> String {
        if !s.isEmpty, s.allSatisfy({ $0.isLetter || $0.isNumber || "-_./=:,+@%".contains($0) }) { return s }
        return "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}
