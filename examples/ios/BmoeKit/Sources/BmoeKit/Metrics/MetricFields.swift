import Foundation

/// What each metrics column means, in one place: the reader's side of the contract the engine
/// writes (docs/telemetry.md). A column added there and not described here still shows, just
/// unexplained, which is the right failure.
public struct MetricField: Equatable, Sendable {
    public enum Better: Sendable { case lower, higher, neutral }

    public let name: String
    public let short: String
    public let measures: String
    public let better: Better
}

public enum MetricFields {
    public static let all: [MetricField] = [
        .init(name: "turn", short: "which generation", measures: "the turn in the session; one file spans them all", better: .neutral),
        .init(name: "step", short: "token index", measures: "this token's number within the turn, from 1", better: .neutral),
        .init(name: "steps", short: "tokens requested", measures: "the answer budget of the turn", better: .neutral),
        .init(name: "wall_ms", short: "token time",
              measures: "the whole time this token took, the one number measured directly; tok/s = 1000/wall_ms", better: .lower),
        .init(name: "compute_ms", short: "compute (a residual)",
              measures: "with streaming on, what is left after the flash and cache terms are subtracted: not measured, so it also absorbs faults and scheduler stalls", better: .lower),
        .init(name: "io_ms", short: "flash read time",
              measures: "time reading experts from flash; under overlap it is summed over the read lanes and can exceed wall_ms", better: .lower),
        .init(name: "stall_ms", short: "wait on flash",
              measures: "overlap only: the wall time during which compute was blocked on a streamed read", better: .lower),
        .init(name: "flash_wait_ms", short: "flash wait",
              measures: "the flash term that is a share of wall_ms, as the server chose it for the run's mode", better: .lower),
        .init(name: "mgmt_ms", short: "cache bookkeeping",
              measures: "time committing, evicting and reordering the expert cache this token", better: .lower),
        .init(name: "read_mib", short: "flash read", measures: "MiB of experts read from flash this token", better: .lower),
        .init(name: "cache_hit_pct", short: "hit rate (cumulative)",
              measures: "share of expert lookups served from the cache since the session started, prefill included", better: .higher),
        .init(name: "cache_budget_mib", short: "cache budget", measures: "the expert cache size this run used", better: .neutral),
        .init(name: "rss_mib", short: "total resident", measures: "everything of this process resident in memory", better: .neutral),
        .init(name: "mem_available_mib", short: "available memory",
              measures: "what the system reports this process may still take", better: .neutral),
        .init(name: "majflt", short: "hard faults",
              measures: "pages the system had dropped and the decode had to read again from flash this token", better: .lower),
        .init(name: "cpu_ms", short: "CPU used",
              measures: "CPU time across all threads; against wall_ms times threads it says compute-bound or stalled", better: .neutral),
        .init(name: "dense_resident_frac", short: "dense weights in memory",
              measures: "a sampled estimate of the share of the non-expert weights still resident", better: .higher),
        .init(name: "mtp_batch", short: "tokens per pass", measures: "tokens a speculative pass confirmed", better: .higher),
    ]

    private static let byName = Dictionary(uniqueKeysWithValues: all.map { ($0.name, $0) })
    public static func of(_ name: String) -> MetricField? { byName[name] }
}
