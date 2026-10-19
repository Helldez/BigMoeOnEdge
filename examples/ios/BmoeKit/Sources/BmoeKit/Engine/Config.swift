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

        init(_ j: JSON) {
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
