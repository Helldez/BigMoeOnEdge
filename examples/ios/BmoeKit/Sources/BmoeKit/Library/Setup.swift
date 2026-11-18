import Foundation

/// How fast a download goes and how long it has left, from the byte counts the transfer reported
/// and when, over a sliding window, so a stall or a burst moves the estimate without making it
/// jump. The web UI's `ui/src/lib/eta.ts`, word for word.
public enum ETA {
    public struct Sample: Equatable, Sendable {
        /// Seconds, any monotonic clock.
        public let at: Double
        public let received: Int64

        public init(at: Double, received: Int64) {
            self.at = at
            self.received = received
        }
    }

    /// Keeps the samples of the last `window` seconds, plus the newest; one with more bytes than
    /// the newest is from before a restart and goes too.
    public static func add(_ samples: [Sample], _ next: Sample, window: Double = 20) -> [Sample] {
        samples.filter { next.at - $0.at <= window && $0.received <= next.received } + [next]
    }

    /// Bytes per second over the window, or nil until two samples a second apart exist.
    public static func rate(_ samples: [Sample]) -> Double? {
        guard samples.count >= 2, let a = samples.first, let b = samples.last else { return nil }
        let dt = b.at - a.at
        guard dt >= 1 else { return nil }
        return max(0, Double(b.received - a.received) / dt)
    }

    /// Seconds left at the current rate, or nil when unknown.
    public static func secondsLeft(_ samples: [Sample], total: Int64) -> Double? {
        guard let r = rate(samples), r > 0, let last = samples.last, total > 0 else { return nil }
        return max(0, Double(total - last.received) / r)
    }

    /// "about 12 min", "about 1 h 20 min", "less than a minute": a person's rounding.
    public static func human(_ s: Double) -> String {
        if s < 60 { return "less than a minute" }
        // Half up, as the web's Math.round.
        let min = Int((s / 60).rounded(.toNearestOrAwayFromZero))
        if min < 60 { return "about \(min) min" }
        let h = min / 60, m = min % 60
        return m > 0 ? "about \(h) h \(m) min" : "about \(h) h"
    }
}

/// The first run (docs/screens.md, "First run"): one path to the first private answer in three
/// real steps, Pick a model, Download, Start. The web UI's `ui/src/routes/Setup.svelte`; what is
/// pure of it lives here.
public enum Setup {
    /// The catalog's entries a first run may offer: its own, with a size.
    static func offered(_ catalog: [CatalogEntry]) -> [CatalogEntry] { catalog.filter { !$0.added && $0.bytes > 0 } }

    /// The one to suggest: the largest that fits in memory, else the smallest.
    public static func recommended(_ catalog: [CatalogEntry], ramBytes: Int64) -> CatalogEntry? {
        let offered = offered(catalog)
        if let fits = offered.filter({ $0.bytes <= ramBytes }).max(by: { $0.bytes < $1.bytes }) { return fits }
        return offered.min { $0.bytes < $1.bytes }
    }

    /// Side by side, the two first runs this product offers: the largest model that fits in memory
    /// (the fastest answers) and the smallest that does not (what streaming from disk is for).
    /// Empty unless both exist and the memory is known.
    public static func pair(_ catalog: [CatalogEntry], ramBytes: Int64) -> [CatalogEntry] {
        guard ramBytes > 0 else { return [] }
        let offered = offered(catalog)
        guard let fits = offered.filter({ $0.bytes <= ramBytes }).max(by: { $0.bytes < $1.bytes }),
              let big = offered.filter({ $0.bytes > ramBytes }).min(by: { $0.bytes < $1.bytes }) else { return [] }
        return [fits, big]
    }

    /// Where the person is, from the model's state, never a button: 3 once the chosen model is on
    /// the device or a load runs, 2 while it downloads or while a paused download of it waits to
    /// resume (unless the person asked to pick another model, `repick`), else 1.
    public static func step(_ state: Library.RowState?, loading: Bool, repick: Bool = false) -> Int {
        if loading || state == .onDevice || state == .loaded { return 3 }
        if state == .downloading || (state == .partial && !repick) { return 2 }
        return 1
    }

    /// Whether the entry fits in memory, and the line that says what that means here.
    public static func fits(_ e: CatalogEntry, ramBytes: Int64) -> Bool { e.bytes <= ramBytes }
    public static func kind(_ e: CatalogEntry, ramBytes: Int64) -> String {
        fits(e, ramBytes: ramBytes) ? "Fits in memory" : "Larger than memory"
    }
    public static func meaning(_ e: CatalogEntry, ramBytes: Int64) -> String {
        fits(e, ramBytes: ramBytes)
            ? "Fits in this phone’s memory: the fastest answers."
            : "Larger than this phone’s memory: it runs, reading the parts it needs from disk as it goes. Slower, and a bigger model."
    }

    /// The facts the app knows about this device, in one line; what it does not know is left out.
    public static func facts(ramMiB: Int64, threads: Int, gpus: [String]) -> String {
        var parts: [String] = []
        if ramMiB > 0 { parts.append("\(Int((Double(ramMiB) / 1024).rounded())) GiB of memory") }
        if threads > 0 { parts.append("\(threads) processor threads") }
        return (parts + gpus.filter { !$0.isEmpty }).joined(separator: " · ")
    }

    /// The questions an empty conversation offers once a model can answer: each sent as it is.
    public static let tries = [
        "Plan three days in Lisbon in March, cheap and walkable.",
        "Explain compound interest with a simple example.",
        "Write a short, friendly email declining a meeting.",
    ]
}
