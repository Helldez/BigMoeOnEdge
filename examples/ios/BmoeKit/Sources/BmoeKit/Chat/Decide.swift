import Foundation

/// Choose from options: the engine's decide request (docs/decide.md) as the chat offers it, through
/// `POST /api/decide`. The options are lettered, the question is the request's prefix and the
/// lettered options its suffix, and the answer is each option with the probability the model put
/// on it. The web UI and the Android app build the same request, so a decision reads alike.
public enum Decide {
    /// Single letters, one token each in the usual tokenizers: two options never share a first token.
    public static let maxOptions = 26

    public static func labels(_ n: Int) -> [String] {
        (0..<min(n, maxOptions)).map { String(UnicodeScalar(UInt8(65 + $0))) }
    }

    /// The options, one per non-blank line.
    public static func parseOptions(_ text: String) -> [String] {
        Array(text.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
            .prefix(maxOptions))
    }

    /// The part of the prompt that changes from one question to the next: the lettered options and
    /// the instruction. The question is the prefix, so asking it again with other options restores
    /// it instead of reading it again.
    public static func suffix(_ options: [String]) -> String {
        var s = "\n\nOptions:\n"
        for (l, o) in zip(labels(options.count), options) { s += "\(l)) \(o)\n" }
        return s + "Answer with the letter of one option only."
    }

    /// How the question reads in the transcript.
    public static func userText(_ question: String, _ options: [String]) -> String {
        zip(labels(options.count), options).reduce(question) { $0 + "\n\($1.0)) \($1.1)" }
    }

    public static func requestBody(_ question: String, _ options: [String]) -> JSON {
        ["prefix": .string(question), "suffix": .string(suffix(options)), "choices": .array(labels(options.count).map(JSON.string))]
    }

    public struct Result: Equatable, Sendable {
        public let scores: [ChoiceScore]
        public let best: Int
        public let metrics: String
        public let cancelled: Bool
    }

    /// The answer, read back against the options it was asked about.
    public static func parse(_ o: JSON, _ options: [String]) -> Result {
        let ls = labels(options.count)
        // exp(log p): the model's probability over its whole vocabulary, so the options need not
        // sum to one. What is missing is mass the model put on something else, and is worth seeing.
        let scores = ls.indices.map { i -> ChoiceScore in
            let lp = o["choice_logp"][i].double ?? -.infinity
            return ChoiceScore(label: ls[i], text: options[i], probability: lp.isFinite ? exp(lp) : 0)
        }
        var metrics = String(format: "prefill %.1fs (%d tok", o["prefill_s"].double(or: 0), o["n_prefilled"].int(or: 0))
        let reused = o["n_reused"].int(or: 0)
        if reused > 0 { metrics += ", \(reused) reused" }
        metrics += ")"
        let kept = o["prefix_state_mib"].double(or: 0)
        if kept > 0 { metrics += String(format: " · context kept %.0f MiB", kept) }
        return Result(scores: scores, best: o["best"].int(or: -1), metrics: metrics, cancelled: o["cancelled"].bool(or: false))
    }
}
