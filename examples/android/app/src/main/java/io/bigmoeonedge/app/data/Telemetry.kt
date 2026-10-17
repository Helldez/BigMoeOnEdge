package io.bigmoeonedge.app.data

import org.json.JSONObject

/**
 * Live per-token metrics, read from the server's `token` and `done` events (the same figures the
 * CLI's --progress lines carry; docs/telemetry.md names them).
 */
data class Telemetry(
    var step: Int = 0,
    var steps: Int = 0,
    var wallMs: Double = 0.0,
    var computeMs: Double = 0.0,
    // The measured, wall-additive flash term, as the server chose it for the run's mode: the blocking
    // read in serial, the wall time compute sat idle on a read under overlap (`flash_wait_ms`). The
    // panel shows it as "flash wait" instead of inverting the clamped compute residual, and without
    // knowing whether the session overlaps.
    var flashWaitMs: Double = 0.0,
    // Cache-management time this token — measured, unlike the legacy compute_ms residual. The
    // four-way breakdown no longer sums to the wall exactly (unattributed is the honest remainder),
    // which is the point: a tidy total was coming from pretending unknown time was compute.
    var mgmtMs: Double = 0.0,
    var cacheHitPct: Double = -1.0,
    // Compute-decomposition of the `computeMs` residual (see docs/telemetry.md). Live per-token
    // values from the token event: `majflt` > 0 means a dense weight re-faulted from flash inside the
    // decode; `cpuMs` ÷ (wallMs × threads) is CPU occupancy — near 1 is compute-bound, well below
    // means a throttled/preempted core. 0 when the platform can't measure them.
    var majflt: Double = 0.0,
    var cpuMs: Double = 0.0,
    // What the CPU figures are read against, from the loaded model in `/api/info` when the token
    // arrived: the compute threads (the compute bar) and every thread busy in a decode (CPU busy).
    // 0 when unknown, which leaves the compute bar empty rather than guessed.
    var computeThreads: Int = 0,
    var busyThreads: Int = 0,
    var text: String = "",
    // The thinking span so far when the model is reasoning, kept apart from [text] so the UI can
    // render it as a distinct block. Empty with thinking off or on a non-reasoning model.
    var reasoning: String = "",
    // Aggregate decode rate over the whole run, parsed from the final summary line; -1 until
    // generation finishes. The per-token [tokensPerSecond] is instantaneous (last token only),
    // so the UI shows this average once it is available.
    var avgTokensPerSecond: Double = -1.0,
    // Per-token AVERAGES over the whole run, from the final summary. Kept for protocol/benchmark
    // compatibility — the panel's compute bar deliberately does NOT read this residual anymore
    // (see breakdown()); compute comes from [avgCpuSPerTok] and what it cannot explain lands in
    // the unattributed term. -1 until generation finishes.
    var avgComputeMs: Double = -1.0,
    var avgMgmtMs: Double = -1.0,
    // The measured flash term of the same run averages, from the done event's flash_wait_s_tok. The
    // summary used to reconstruct flash wait from the clamped compute residual instead — against the
    // documented contract — which over-attributed to flash exactly when compute_ms had been clamped
    // to 0 (issue #98). -1 until generation finishes.
    var avgFlashWaitMs: Double = -1.0,
    // End-of-run figures from the done event; -1 / 0 until generation finishes.
    var prefillTps: Double = -1.0,      // prompt prefill rate (tok/s)
    var ttftS: Double = -1.0,           // time-to-first-token = model load + prompt prefill (s)
    var readMib: Double = -1.0,         // total flash streamed this generation (MiB)
    var cacheResidentMib: Double = -1.0, // expert cache resident size (MiB)
    var cacheBudgetMib: Double = -1.0,  // expert cache budget (MiB); fixed for the run
    // Run averages of the compute decomposition, from the done event; -1 until done.
    var avgMajfltPerTok: Double = -1.0, // major page faults per token over the run
    var avgCpuSPerTok: Double = -1.0,   // CPU-seconds per token (summed across threads) over the run
    // Self-speculation counters from the done event; all 0 when speculation was off. Without these the UI
    // cannot tell whether it ran at all, let alone whether it earned its keep. They describe the
    // loop, not a source: the n-gram lookup and the MTP head report through the same fields.
    var mtpDrafted: Long = 0,
    var mtpAccepted: Long = 0,
    var mtpDecodes: Long = 0,
    // Passes that guessed anything. Below mtpDecodes it means the source abstained on the rest,
    // which ran at exactly the unspeculated cost — only the n-gram source ever does that.
    var draftedSteps: Long = 0,
    // Seconds per token spent drafting, and the whole between-decode gap. tok/s counts decode time
    // ONLY, so these are time the user waits that the headline rate does not include.
    var mtpDraftSPerTok: Double = 0.0,
    var loopOverheadSPerTok: Double = 0.0,
) {
    val tokensPerSecond: Double get() = if (wallMs > 0) 1000.0 / wallMs else 0.0

    /** Share of drafts the model itself confirmed, or -1 when nothing was drafted. */
    val mtpAcceptancePct: Double get() =
        if (mtpDrafted > 0) 100.0 * mtpAccepted / mtpDrafted else -1.0

    /** Tokens confirmed per verify pass — what the speculation actually bought. */
    val mtpTokensPerDecode: Double get() =
        if (mtpDecodes > 0 && step > 0) step.toDouble() / mtpDecodes else -1.0

    /**
     * The rate the user actually experiences: decode time PLUS the gap between decodes, where
     * drafting lives. Reporting only the decode rate flatters speculation, because the drafting it
     * adds happens outside the measured window.
     */
    val effectiveTokensPerSecond: Double get() {
        if (avgTokensPerSecond <= 0) return -1.0
        val perTok = 1.0 / avgTokensPerSecond + loopOverheadSPerTok
        return if (perTok > 0) 1.0 / perTok else -1.0
    }
}

/**
 * One token's time, split into the four terms the panel draws — compute, flash wait, cache mgmt
 * and the unattributed remainder — plus the diagnostics that explain them. Derived by [breakdown];
 * see MetricFields for the same contract as the CSV states it.
 */
data class Breakdown(
    val wallMs: Double,
    val computeMs: Double,
    val flashWaitMs: Double,
    val mgmtMs: Double,
    // Wall time none of the three measured terms explains (compute, flash wait, cache mgmt):
    // zram swap-in, preemption, frequency caps — the off-CPU time the old three-bar panel painted
    // as compute and read as "the model is thinking". Clamped at 0: measurement noise is allowed
    // to make the terms overlap the wall, and the bars are not rescaled to force a tidy total.
    val unattributedMs: Double,
    /** These are run averages, not the last token — the panel labels them "avg". */
    val isAverage: Boolean,
    /** CPU-time ÷ (wall × busy threads), or -1 when the platform couldn't measure it. */
    val cpuBusyPct: Double,
    /** Major faults per token, or -1 when unmeasured. */
    val faultsPerToken: Double,
) {
    /** Denominator for the meter bars: the wall time, or the four terms before it is known. */
    val totalMs: Double get() =
        if (wallMs > 0.0) wallMs else computeMs + flashWaitMs + mgmtMs + unattributedMs
}

/**
 * Split a token's wall time into compute / flash-wait / cache-mgmt / unattributed.
 *
 * Both branches — live (last token) and end-of-run (averages) — now derive the SAME way (issue
 * #98; they used to disagree, and the end-of-run one inverted the clamped compute residual, which
 * the telemetry contract warns against). Flash wait is always the MEASURED wall-additive read
 * term the server reports (stall under overlap, io in serial). Compute is process CPU time over
 * the compute threads — a measured
 * attribution proxy for matmul work, not the legacy `compute_ms` residual, which by definition
 * absorbs everything unmeasured (zram swap-in, preemption, faults); what that residual used to
 * hide lands in [Breakdown.unattributedMs] instead. A missing measurement contributes 0 to its
 * bar and its time stays unattributed — it is never reconstructed from the wall.
 *
 * The busy threads (CPU-busy diagnostic) include the I/O lanes under overlap — the CPU numerator
 * is whole-process. The compute threads (displayed compute) divide that same numerator down to a
 * per-compute-thread figure; the two denominators are different on purpose and must not be
 * "simplified" into one. Both are the server's (`busy_threads`, `compute_threads`), never guessed.
 */
fun breakdown(t: Telemetry): Breakdown {
    val busyThreads = t.busyThreads
    val computeThreads = t.computeThreads
    // Summary mode is gated by the summary itself, not by the legacy residual: the bars no longer
    // read avgComputeMs at all, so it must not control which branch they take either.
    val useAvg = t.avgTokensPerSecond > 0
    val mgmt = (if (useAvg) t.avgMgmtMs else t.mgmtMs).coerceAtLeast(0.0)
    val wall = if (useAvg) 1000.0 / t.avgTokensPerSecond else t.wallMs
    // Every component is measured or zero. A missing measurement is NOT reconstructed from the
    // wall — that is exactly the attribution error #98 exists to fix (the residual silently
    // absorbed zram, preemption, faults as "compute"). What is unmeasured stays unattributed.
    val flashWait = (if (useAvg) t.avgFlashWaitMs else t.flashWaitMs).coerceAtLeast(0.0)
    val cpuMs = if (useAvg) {
        if (t.avgCpuSPerTok >= 0) t.avgCpuSPerTok * 1000.0 else 0.0
    } else {
        t.cpuMs.coerceAtLeast(0.0)
    }
    val compute = if (computeThreads > 0) cpuMs / computeThreads else 0.0
    val unattributed = (wall - compute - flashWait - mgmt).coerceAtLeast(0.0)

    val useAvgCpu = useAvg && t.avgCpuSPerTok >= 0
    val cpuSPerTok = if (useAvgCpu) t.avgCpuSPerTok else t.cpuMs / 1000.0
    val cpuBusy = if (cpuSPerTok > 0.0 && wall > 0.0 && busyThreads > 0) {
        cpuSPerTok / (wall / 1000.0 * busyThreads) * 100.0
    } else {
        -1.0
    }
    return Breakdown(
        wallMs = wall,
        computeMs = compute,
        flashWaitMs = flashWait,
        mgmtMs = mgmt,
        unattributedMs = unattributed,
        isAverage = useAvg,
        cpuBusyPct = cpuBusy,
        faultsPerToken = if (useAvgCpu) t.avgMajfltPerTok else t.majflt,
    )
}

/** The live fields of one `token` event, applied to [t] (the text is the chat's, not here). */
fun Telemetry.applyToken(o: JSONObject): Telemetry = apply {
    step = o.optInt("step")
    steps = o.optInt("steps")
    wallMs = o.optDouble("wall_ms", 0.0)
    computeMs = o.optDouble("compute_ms", 0.0)
    flashWaitMs = o.optDouble("flash_wait_ms", 0.0)
    mgmtMs = o.optDouble("mgmt_ms", 0.0)
    cacheHitPct = o.optDouble("cache_hit_pct", -1.0)
    majflt = o.optDouble("majflt", 0.0)
    cpuMs = o.optDouble("cpu_ms", 0.0)
}

/** The run averages of one `done` event, applied to [t]. */
fun Telemetry.applyDone(o: JSONObject): Telemetry = apply {
    avgTokensPerSecond = o.optDouble("tok_s", -1.0)
    avgComputeMs = o.optDouble("compute_s_tok", -0.001) * 1000.0
    avgMgmtMs = o.optDouble("mgmt_s_tok", -0.001) * 1000.0
    avgFlashWaitMs = o.optDouble("flash_wait_s_tok", -0.001) * 1000.0
    prefillTps = o.optDouble("prefill_tps", -1.0)
    val loadS = o.optDouble("load_s", -1.0)
    val prefillS = o.optDouble("prefill_s", -1.0)
    ttftS = if (loadS >= 0 && prefillS >= 0) loadS + prefillS else -1.0
    readMib = o.optDouble("read_mib", -1.0)
    cacheResidentMib = o.optDouble("cache_resident_mib", -1.0)
    cacheBudgetMib = o.optDouble("cache_budget_mib", -1.0)
    avgMajfltPerTok = o.optDouble("majflt_tok", -1.0)
    avgCpuSPerTok = o.optDouble("cpu_s_tok", -1.0)
    mtpDrafted = o.optLong("mtp_drafted", 0)
    mtpAccepted = o.optLong("mtp_accepted", 0)
    mtpDecodes = o.optLong("mtp_decodes", 0)
    draftedSteps = o.optLong("drafted_steps", 0)
    mtpDraftSPerTok = o.optDouble("mtp_draft_s_tok", 0.0)
    loopOverheadSPerTok = o.optDouble("loop_overhead_s_tok", 0.0)
    if (o.optInt("tokens", 0) > 0) step = o.optInt("tokens")
}
