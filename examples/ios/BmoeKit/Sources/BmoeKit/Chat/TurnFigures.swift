import Foundation

/// The figures of a finished answer, from the turn's own summary (the server's `done` event), in
/// the words every UI uses for the same quantity (docs/screens.md, "The figures of an answer"): one
/// quiet line under the answer, tg and pp, which opens to each figure with what it measures. Only
/// one figure is a set of percentages, and it adds up to 100: where each token's time went. What was
/// found in memory is another quantity, so it is said as a count ("72 of every 100 parts"), never
/// as a percentage beside the time shares a person would try to add up. The web UI's
/// `ui/src/lib/turnMetrics.ts`, word for word.
public struct TurnSummary: Equatable, Sendable {
    public let tokS: Double
    public let prefillTps: Double
    public let prefillS: Double
    public let nPrompt: Int
    public let nPast: Int
    public let cacheHitPct: Double
    /// Seconds per token computing, waiting for the disk and managing the cache.
    public let computeSTok: Double
    public let flashWaitSTok: Double
    public let mgmtSTok: Double
    public let cancelled: Bool
    public let mtpDecodes: Int64
    public let mtpDrafted: Int64
    public let mtpAccepted: Int64

    public init(_ o: JSON) {
        tokS = o["tok_s"].double(or: 0)
        prefillTps = o["prefill_tps"].double(or: 0)
        prefillS = o["prefill_s"].double(or: 0)
        nPrompt = o["n_prompt"].int(or: 0)
        nPast = o["n_past"].int(or: -1)
        cacheHitPct = o["cache_hit_pct"].double(or: -1)
        computeSTok = o["compute_s_tok"].double(or: 0)
        flashWaitSTok = o["flash_wait_s_tok"].double(or: 0)
        mgmtSTok = o["mgmt_s_tok"].double(or: 0)
        cancelled = o["cancelled"].bool(or: false)
        mtpDecodes = o["mtp_decodes"].int64(or: 0)
        mtpDrafted = o["mtp_drafted"].int64(or: 0)
        mtpAccepted = o["mtp_accepted"].int64(or: 0)
    }

    /// How fast the prompt was read: the server's rate, or the tokens over the seconds; 0 unknown.
    public var promptTokS: Double {
        if prefillTps > 0 { return prefillTps }
        return nPrompt > 0 && prefillS > 0 ? Double(nPrompt) / prefillS : 0
    }
}

public struct TurnFigure: Equatable, Sendable, Identifiable {
    public enum ID: String, Sendable { case speed, time, cache, prompt, context, drafts, stopped }
    public let id: ID
    public let label: String
    public let value: String
    /// What the figure measures, in plain words: shown when the person opens the figures.
    public let hint: String

    /// Longer than this, a value goes on its own line under its label, left-aligned, rather than
    /// wrapping as a ragged column beside it (the rule of every UI of this project).
    public static let besideLabelMax = 24

    /// Whether the value is shown under its label instead of at the right of it.
    public var valueOnOwnLine: Bool { Self.valueOnOwnLine(value) }
    public static func valueOnOwnLine(_ value: String) -> Bool { value.count > besideLabelMax }
}

/// One part of a token's time, as a whole percentage.
public struct TimeShare: Equatable, Sendable {
    public enum Key: String, Sendable { case compute, disk, mgmt, other }
    public let key: Key
    public let pct: Int

    /// The part in the figure's words: "computing 70%".
    public var text: String {
        switch key {
        case .compute: return "computing \(pct)%"
        case .disk: return "waiting for the disk \(pct)%"
        case .mgmt: return "managing the cache \(pct)%"
        case .other: return "other \(pct)%"
        }
    }
}

public enum TurnFigures {
    /// The line under a finished answer: `tg 12.4 tok/s · pp 140 tok/s`, pp left out when unknown.
    public static func line(_ s: TurnSummary) -> String {
        var parts = ["tg \(fixed(s.tokS, 1)) tok/s"]
        let pp = s.promptTokS
        if pp > 0 { parts.append("pp \(fixed(pp, 0)) tok/s") }
        return parts.joined(separator: " · ")
    }

    /// The same place while an answer streams: the rate the engine writes at now.
    public static func liveLine(rate: Double) -> String { "tg \(fixed(rate, 1)) tok/s" }

    /// Whole percentages of `values` that add up to exactly 100 (largest remainder), in their
    /// order; empty when they sum to nothing. Ties go to the earlier part, as the web's stable sort.
    public static func wholeShares(_ values: [Double]) -> [Int] {
        let total = values.reduce(0, +)
        guard total > 0 else { return [] }
        let exact = values.map { 100 * $0 / total }
        var pct = exact.map { Int($0.rounded(.down)) }
        var left = 100 - pct.reduce(0, +)
        let order = exact.indices.sorted { a, b in
            let ra = exact[a] - exact[a].rounded(.down), rb = exact[b] - exact[b].rounded(.down)
            return ra != rb ? ra > rb : a < b
        }
        for i in order where left > 0 {
            pct[i] += 1
            left -= 1
        }
        return pct
    }

    /// Where each token's time went: computing, waiting for the disk, managing the cache, and what
    /// no measured term claims, adding up to 100; parts that round to nothing left out. Empty
    /// without a rate.
    public static func timeShares(_ s: TurnSummary) -> [TimeShare] {
        guard s.tokS > 0 else { return [] }
        let wall = 1 / s.tokS
        let compute = max(0, s.computeSTok), disk = max(0, s.flashWaitSTok), mgmt = max(0, s.mgmtSTok)
        let other = max(0, wall - compute - disk - mgmt)
        let keys: [TimeShare.Key] = [.compute, .disk, .mgmt, .other]
        let pct = wholeShares([compute, disk, mgmt, other])
        guard !pct.isEmpty else { return [] }
        return zip(keys, pct).map { TimeShare(key: $0, pct: $1) }.filter { $0.pct > 0 }
    }

    /// Every figure of the turn, in order, each only when the turn measured it.
    public static func figures(_ s: TurnSummary, nCtx: Int, expectedTokS: Double?) -> [TurnFigure] {
        var out: [TurnFigure] = []
        let expected = expectedTokS.flatMap { $0 > 0 ? ", plan expected \(fixed($0, 1))" : nil } ?? ""
        out.append(TurnFigure(id: .speed, label: "Answer speed", value: "\(fixed(s.tokS, 1)) tok/s" + expected,
                              hint: "Tokens written per second. A token is a piece of a word: about three quarters of a word in "
                                  + "English. The plan expected rate is what the plan predicted for this phone."))
        let shares = timeShares(s)
        if !shares.isEmpty {
            out.append(TurnFigure(id: .time, label: "Where each token's time went",
                                  value: shares.map(\.text).joined(separator: " · "),
                                  hint: "The time it took to write each token, split into what it was spent on. The parts add "
                                      + "up to 100%."))
        }
        if s.cacheHitPct >= 0 {
            out.append(TurnFigure(id: .cache, label: "Found in memory", value: "\(Int(s.cacheHitPct.rounded())) of every 100 parts",
                                  hint: "Of the parts of the model each token needed, how many were already in memory instead "
                                      + "of read from the disk (the expert cache). More is faster."))
        }
        if s.prefillS > 0 {
            out.append(TurnFigure(id: .prompt, label: "Reading the message",
                                  value: "\(fixed(s.promptTokS, 1)) tok/s, \(max(0, s.nPrompt)) tokens in \(fixed(s.prefillS, 1)) s",
                                  hint: "How fast the engine read what was new in this turn before writing. Earlier turns kept "
                                      + "in the context are not read again."))
        }
        if s.nPast >= 0, nCtx > 0 {
            out.append(TurnFigure(id: .context, label: "Context used", value: "\(grouped(s.nPast)) of \(grouped(nCtx)) tokens",
                                  hint: "How much of the model's memory for text (its context) this conversation takes. When "
                                      + "it is full, the conversation cannot go on: start a new one, or raise the context in "
                                      + "Settings."))
        }
        if s.mtpDecodes > 0, s.mtpDrafted > 0 {
            out.append(TurnFigure(id: .drafts, label: "Guesses kept", value: "\(s.mtpAccepted) of \(s.mtpDrafted) guesses",
                                  hint: "The share of tokens guessed ahead that the model confirmed. More kept means faster "
                                      + "answers."))
        }
        if s.cancelled {
            out.append(TurnFigure(id: .stopped, label: "Stopped", value: "", hint: "The answer was stopped before it finished."))
        }
        return out
    }

    /// Fixed decimals with a point, whatever the device's locale: the web UI's `toFixed`, which
    /// rounds a half up (1.25 to "1.3") where printf would round it to even.
    public static func fixed(_ v: Double, _ digits: Int) -> String {
        guard v.isFinite else { return "-" }
        let scale = pow(10, Double(digits))
        let r = (v * scale).rounded(.toNearestOrAwayFromZero) / scale
        return String(format: "%.\(digits)f", locale: Locale(identifier: "en_US_POSIX"), r)
    }

    /// Thousands grouped with commas, as the web UI's `toLocaleString('en')`.
    public static func grouped(_ n: Int) -> String {
        let f = NumberFormatter()
        f.locale = Locale(identifier: "en_US")
        f.numberStyle = .decimal
        return f.string(from: NSNumber(value: n)) ?? String(n)
    }
}

/// The few plain words beside the logo at the top of the app (docs/design.md, "The seal"): what
/// the engine and the open conversation are doing, taken from their state, never a figure.
public enum StatusWords {
    /// A share of each token's time spent waiting for the disk above which the line says so.
    public static let diskShare = 0.3

    public struct Input: Sendable {
        public var reachable: Bool
        /// The engine's state, nil before the first one arrives.
        public var engine: EngineState?
        /// An answer is being written in the open conversation.
        public var running: Bool
        /// The live token's wait for the disk and its wall time, in ms.
        public var flashWaitMs: Double
        public var wallMs: Double
        /// The open conversation's last item is an answer, and whether it has words yet.
        public var lastIsAnswer: Bool
        public var answerHasText: Bool
        public var conversationFailed: Bool
        /// The last finished turn was stopped.
        public var stopped: Bool

        public init(reachable: Bool, engine: EngineState?, running: Bool = false, flashWaitMs: Double = 0, wallMs: Double = 0,
                    lastIsAnswer: Bool = false, answerHasText: Bool = false, conversationFailed: Bool = false,
                    stopped: Bool = false) {
            self.reachable = reachable
            self.engine = engine
            self.running = running
            self.flashWaitMs = flashWaitMs
            self.wallMs = wallMs
            self.lastIsAnswer = lastIsAnswer
            self.answerHasText = answerHasText
            self.conversationFailed = conversationFailed
            self.stopped = stopped
        }
    }

    /// Working: the logo breathes. Alert: the words take the warning's colour.
    public enum Mood: Sendable { case idle, busy, alert, sleep }

    /// The line under the logo on an empty conversation: the same words, but "Ready" is the
    /// promise that nothing leaves the phone, said once a model can answer.
    public static let greeting = "Private: everything stays on this phone."

    public static func say(_ i: Input, greet: Bool = false) -> (text: String, mood: Mood) {
        guard i.reachable else { return ("The engine is not running.", .sleep) }
        guard let state = i.engine else { return ("Connecting…", .busy) }
        switch state {
        case .planning: return ("Measuring this phone…", .busy)
        case .loading: return ("Loading the model…", .busy)
        case .error: return ("The model did not load. See Models.", .alert)
        case .empty, .ready: break
        }
        if i.running {
            if i.wallMs > 0, i.flashWaitMs / i.wallMs > diskShare { return ("Reading part of the model from the disk…", .busy) }
            return (i.lastIsAnswer && i.answerHasText ? "Writing…" : "Thinking…", .busy)
        }
        if state == .empty { return ("No model loaded", .sleep) }
        if i.conversationFailed { return ("Something went wrong; see the note below", .alert) }
        if i.stopped, i.lastIsAnswer { return ("Stopped", .idle) }
        if i.lastIsAnswer { return ("Answered", .idle) }
        return (greet ? greeting : "Ready", .idle)
    }
}
