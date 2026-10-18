import Foundation

/// A per-token metrics CSV, read back: columns by name, the `# bmoe_metrics` preamble (what the run
/// was) and the per-turn `# summary` lines (what each turn did). Everything is read by name, so a
/// file from an older build, with fewer columns, still shows what it has.
public struct MetricsFile: Equatable, Sendable {
    public let header: [String]
    public let rows: [[String]]
    public let summaries: [[String: String]]
    public let info: [String: String]

    /// Columns where -1 means "not sampled", not a value.
    static let notSampledMinusOne: Set<String> = ["cache_hit_pct", "dense_resident_frac"]

    public init(text: String) {
        let lines = text.split(whereSeparator: \.isNewline).map(String.init)
        let data = lines.filter { !$0.isEmpty && !$0.hasPrefix("#") }
        header = data.first?.split(separator: ",", omittingEmptySubsequences: false).map(String.init) ?? []
        rows = data.dropFirst().map { $0.split(separator: ",", omittingEmptySubsequences: false).map(String.init) }
        summaries = lines.filter { $0.hasPrefix("# summary") }.map(Self.fields)
        var info: [String: String] = [:]
        for l in lines where l.hasPrefix("# ") && !l.hasPrefix("# summary") {
            for (k, v) in Self.fields(l) { info[k] = v }
        }
        self.info = info
    }

    public init(contentsOf url: URL) throws {
        self.init(text: try String(contentsOf: url, encoding: .utf8))
    }

    /// Whitespace-separated `key=value` pairs, order-independent, unknown keys kept.
    static func fields(_ line: String) -> [String: String] {
        var out: [String: String] = [:]
        for tok in line.split(whereSeparator: { $0 == " " || $0 == "\t" }) {
            let parts = tok.split(separator: "=", maxSplits: 1)
            if parts.count == 2, !parts[0].isEmpty { out[String(parts[0])] = String(parts[1]) }
        }
        return out
    }

    /// How the recorder writes a parameter's key in the preamble.
    public static func preambleKey(_ paramKey: String) -> String { paramKey.replacingOccurrences(of: "-", with: "_") }

    /// This column's values; nil where the cell is not a number or is a "not sampled" sentinel, so
    /// every column stays aligned by row.
    public func column(_ name: String) -> [Double?]? {
        guard let i = header.firstIndex(of: name) else { return nil }
        let sentinel = Self.notSampledMinusOne.contains(name)
        return rows.map { row in
            guard i < row.count, let v = Double(row[i]) else { return nil }
            return sentinel && v == -1 ? nil : v
        }
    }

    /// Columns worth plotting: numeric, and not an axis or a label.
    public var plottable: [String] {
        header.filter { !["step", "steps", "turn"].contains($0) && (column($0)?.contains { $0 != nil } ?? false) }
    }

    /// A short name for the run: the model and each turn's rate.
    public var title: String {
        let rates = summaries.compactMap { $0["tok/s"].flatMap(Double.init) }.map { String(format: "%.2f", $0) }
        let parts = [info["model"] ?? "", rates.isEmpty ? "" : rates.joined(separator: " → ") + " tok/s"]
        return parts.filter { !$0.isEmpty }.joined(separator: " · ")
    }
}

/// Mean, extremes and count of the values a column has.
public struct ColumnStats: Equatable, Sendable {
    public let count: Int
    public let min: Double
    public let mean: Double
    public let max: Double

    public init?(_ values: [Double?]) {
        let v = values.compactMap { $0 }
        guard !v.isEmpty else { return nil }
        count = v.count
        min = v.min()!
        max = v.max()!
        mean = v.reduce(0, +) / Double(v.count)
    }
}
