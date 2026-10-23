import Foundation

/// Live per-token figures, read from the server's `token` and `done` events (docs/telemetry.md
/// names them). Before a run ends they are the last token's; after, the run's averages.
public struct Telemetry: Equatable, Sendable {
    public var step = 0
    public var steps = 0
    public var wallMs = 0.0
    /// The measured flash term that is a share of the wall, as the server chose it for the run's
    /// mode (`flash_wait_ms`): shown as "flash wait" without knowing whether the session overlaps.
    public var flashWaitMs = 0.0
    public var mgmtMs = 0.0
    public var cacheHitPct = -1.0
    public var majflt = 0.0
    public var cpuMs = 0.0
    /// What the CPU figures are read against, from the session that produced them; 0 when unknown,
    /// which leaves the compute bar empty rather than guessed.
    public var computeThreads = 0
    public var busyThreads = 0

    // The run, from the `done` event; -1 until it ends.
    public var avgTokensPerSecond = -1.0
    public var avgMgmtMs = -1.0
    public var avgFlashWaitMs = -1.0
    public var prefillTps = -1.0
    /// This turn's time to first token, as the server measures it (`ttft_s`). Never the model's load,
    /// which only the first turn after it waited for and which made every later turn read as slow.
    public var ttftSeconds = -1.0
    public var readMiB = -1.0
    public var cacheResidentMiB = -1.0
    public var cacheBudgetMiB = -1.0
    public var avgMajfltPerToken = -1.0
    public var avgCpuSecondsPerToken = -1.0
    // Speculation, all 0 when it was off.
    public var mtpDrafted: Int64 = 0
    public var mtpAccepted: Int64 = 0
    public var mtpDecodes: Int64 = 0
    public var loopOverheadSecondsPerToken = 0.0

    public init() {}

    public var finished: Bool { avgTokensPerSecond > 0 }
    public var tokensPerSecond: Double { wallMs > 0 ? 1000 / wallMs : 0 }
    /// The rate shown: the last token's while generating, the run's average after.
    public var rate: Double { finished ? avgTokensPerSecond : tokensPerSecond }
    public var mtpAcceptancePct: Double { mtpDrafted > 0 ? 100 * Double(mtpAccepted) / Double(mtpDrafted) : -1 }

    /// The rate the person waits at: decode time plus the gap between decodes, where drafting lives.
    public var effectiveTokensPerSecond: Double {
        guard avgTokensPerSecond > 0 else { return -1 }
        let perToken = 1 / avgTokensPerSecond + loopOverheadSecondsPerToken
        return perToken > 0 ? 1 / perToken : -1
    }

    public mutating func apply(token o: JSON) {
        step = o["step"].int(or: 0)
        steps = o["steps"].int(or: 0)
        wallMs = o["wall_ms"].double(or: 0)
        flashWaitMs = o["flash_wait_ms"].double(or: 0)
        mgmtMs = o["mgmt_ms"].double(or: 0)
        cacheHitPct = o["cache_hit_pct"].double(or: -1)
        majflt = o["majflt"].double(or: 0)
        cpuMs = o["cpu_ms"].double(or: 0)
    }

    public mutating func apply(done o: JSON) {
        avgTokensPerSecond = o["tok_s"].double(or: -1)
        avgMgmtMs = o["mgmt_s_tok"].double(or: -0.001) * 1000
        avgFlashWaitMs = o["flash_wait_s_tok"].double(or: -0.001) * 1000
        prefillTps = o["prefill_tps"].double(or: -1)
        ttftSeconds = o["ttft_s"].double(or: o["prefill_s"].double(or: -1))
        readMiB = o["read_mib"].double(or: -1)
        cacheResidentMiB = o["cache_resident_mib"].double(or: -1)
        cacheBudgetMiB = o["cache_budget_mib"].double(or: -1)
        avgMajfltPerToken = o["majflt_tok"].double(or: -1)
        avgCpuSecondsPerToken = o["cpu_s_tok"].double(or: -1)
        mtpDrafted = o["mtp_drafted"].int64(or: 0)
        mtpAccepted = o["mtp_accepted"].int64(or: 0)
        mtpDecodes = o["mtp_decodes"].int64(or: 0)
        loopOverheadSecondsPerToken = o["loop_overhead_s_tok"].double(or: 0)
        let tokens = o["tokens"].int(or: 0)
        if tokens > 0 { step = tokens }
    }

    /// One token's time split into the four terms the live card draws (docs/telemetry.md): compute
    /// is process CPU time over the compute threads, flash wait the server's measured term, cache
    /// management measured, and the remainder unattributed, never painted as compute. A missing
    /// measurement contributes 0 and its time stays unattributed: it is never rebuilt from the wall.
    public var breakdown: Breakdown {
        let avg = finished
        let wall = avg ? 1000 / avgTokensPerSecond : wallMs
        let flash = max(0, avg ? avgFlashWaitMs : flashWaitMs)
        let mgmt = max(0, avg ? avgMgmtMs : mgmtMs)
        let cpu = avg ? (avgCpuSecondsPerToken >= 0 ? avgCpuSecondsPerToken * 1000 : 0) : max(0, cpuMs)
        let compute = computeThreads > 0 ? cpu / Double(computeThreads) : 0
        // The busy threads include the read lanes under overlap; the compute threads divide the same
        // numerator down to one compute thread. Two denominators on purpose.
        let cpuBusy = cpu > 0 && wall > 0 && busyThreads > 0 ? cpu / (wall * Double(busyThreads)) * 100 : -1
        return Breakdown(wallMs: wall, computeMs: compute, flashWaitMs: flash, mgmtMs: mgmt,
                         unattributedMs: max(0, wall - compute - flash - mgmt), isAverage: avg, cpuBusyPct: cpuBusy,
                         faultsPerToken: avg && avgCpuSecondsPerToken >= 0 ? avgMajfltPerToken : majflt)
    }
}

public struct Breakdown: Equatable, Sendable {
    public let wallMs: Double
    public let computeMs: Double
    public let flashWaitMs: Double
    public let mgmtMs: Double
    public let unattributedMs: Double
    /// Run averages, not the last token: the card says "avg".
    public let isAverage: Bool
    /// CPU time over wall times busy threads, or -1 when unmeasured.
    public let cpuBusyPct: Double
    public let faultsPerToken: Double

    /// What the bars are drawn against: the wall, or the four terms before it is known.
    public var totalMs: Double { wallMs > 0 ? wallMs : computeMs + flashWaitMs + mgmtMs + unattributedMs }
}
