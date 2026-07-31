package io.bigmoeonedge.example

import org.json.JSONObject

/** Live per-token metrics parsed from bmoe-cli's --progress output. */
data class Telemetry(
    var step: Int = 0,
    var steps: Int = 0,
    var wallMs: Double = 0.0,
    var computeMs: Double = 0.0,
    // The measured, wall-additive flash term: io_ms in serial (the blocking read), stall_ms under
    // overlap (the wall time compute sat idle on a read). The panel shows the one that matches the
    // run's mode as "flash wait" instead of inverting the clamped compute residual, which mislabels
    // the token when compute_ms was clamped to 0.
    var ioMs: Double = 0.0,
    var stallMs: Double = 0.0,
    // Cache-management time this token — the third wall-additive term. wall = compute + flash-wait +
    // mgmt exactly (the engine defines compute as that residual), so the panel can show a breakdown
    // that sums to the token time and makes tok/s = 1000/wall self-evident.
    var mgmtMs: Double = 0.0,
    var cacheHitPct: Double = -1.0,
    // Compute-decomposition of the `computeMs` residual (see docs/telemetry.md). Live per-token
    // values from BMOE_PROGRESS: `majflt` > 0 means a dense weight re-faulted from flash inside the
    // decode; `cpuMs` ÷ (wallMs × threads) is CPU occupancy — near 1 is compute-bound, well below
    // means a throttled/preempted core. 0 when the platform can't measure them.
    var majflt: Double = 0.0,
    var cpuMs: Double = 0.0,
    var text: String = "",
    // The thinking span so far when the model is reasoning, kept apart from [text] so the UI can
    // render it as a distinct block. Empty with thinking off or on a non-reasoning model.
    var reasoning: String = "",
    // Aggregate decode rate over the whole run, parsed from the final summary line; -1 until
    // generation finishes. The per-token [tokensPerSecond] is instantaneous (last token only),
    // so the UI shows this average once it is available.
    var avgTokensPerSecond: Double = -1.0,
    // Per-token AVERAGES over the whole run, from the final summary — shown at the end instead of
    // the last token's instantaneous [computeMs]. -1 until generation finishes.
    var avgComputeMs: Double = -1.0,
    var avgMgmtMs: Double = -1.0,
    // End-of-run figures from the final summary (BMOE_DONE); -1 / 0 until generation finishes.
    var prefillTps: Double = -1.0,      // prompt prefill rate (tok/s)
    var ttftS: Double = -1.0,           // time-to-first-token = model load + prompt prefill (s)
    var readMib: Double = -1.0,         // total flash streamed this generation (MiB)
    var cacheResidentMib: Double = -1.0, // expert cache resident size (MiB)
    var cacheBudgetMib: Double = -1.0,  // expert cache budget (MiB); fixed for the run
    // Run averages of the compute decomposition, from the final summary (BMOE_DONE); -1 until done.
    var avgMajfltPerTok: Double = -1.0, // major page faults per token over the run
    var avgCpuSPerTok: Double = -1.0,   // CPU-seconds per token (summed across threads) over the run
    // Self-speculation counters from BMOE_DONE; all 0 when speculation was off. Without these the UI
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
 * One token's time, split into the three wall-additive terms the panel draws, plus the diagnostics
 * that explain the compute term. Derived by [breakdown]; see MetricFields for the same contract as
 * the CSV states it.
 */
data class Breakdown(
    val wallMs: Double,
    val computeMs: Double,
    val flashWaitMs: Double,
    val mgmtMs: Double,
    /** These are run averages, not the last token — the panel labels them "avg". */
    val isAverage: Boolean,
    /** CPU-time ÷ (wall × busy threads), or -1 when the platform couldn't measure it. */
    val cpuBusyPct: Double,
    /** Major faults per token, or -1 when unmeasured. */
    val faultsPerToken: Double,
) {
    /** Denominator for the meter bars: the wall time, or the terms themselves before it is known. */
    val totalMs: Double get() = if (wallMs > 0.0) wallMs else computeMs + flashWaitMs + mgmtMs
}

/**
 * Split a token's wall time into compute / flash-wait / cache-mgmt.
 *
 * While generating this reads the live last token; once the run has a summary it switches to the
 * run averages. The two derive the split differently on purpose. Live, flash wait is the MEASURED
 * wall-additive read term — stall_ms under overlap (the wall time compute sat idle), io_ms in
 * serial (the blocking read) — and compute is the leftover, which keeps the clamp on compute so a
 * near-0 compute stays honest instead of being dumped into "flash wait". The end-of-run average
 * has no per-mode io/stall to read, so it keeps the residual form.
 *
 * [busyThreads] must include the I/O lanes under overlap: the CPU numerator is whole-process, so a
 * denominator that counts only compute threads reads occupancy above 100%.
 */
fun breakdown(t: Telemetry, overlap: Boolean, busyThreads: Int): Breakdown {
    val useAvg = t.avgTokensPerSecond > 0 && t.avgComputeMs >= 0
    val mgmt = if (useAvg) t.avgMgmtMs.coerceAtLeast(0.0) else t.mgmtMs
    val wall = if (useAvg) {
        if (t.avgTokensPerSecond > 0) 1000.0 / t.avgTokensPerSecond else 0.0
    } else {
        t.wallMs
    }
    val compute: Double
    val flashWait: Double
    if (useAvg) {
        compute = t.avgComputeMs
        flashWait = (wall - compute - mgmt).coerceAtLeast(0.0)
    } else {
        flashWait = if (overlap) t.stallMs else t.ioMs
        compute = (wall - flashWait - mgmt).coerceAtLeast(0.0)
    }

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
        isAverage = useAvg,
        cpuBusyPct = cpuBusy,
        faultsPerToken = if (useAvgCpu) t.avgMajfltPerTok else t.majflt,
    )
}

/**
 * Incrementally parses the CLI's per-token telemetry contract (see docs/telemetry.md):
 *   BMOE_LOAD     {"mb":..,"ms":..}
 *   BMOE_PROGRESS {"step":..,"steps":..,"wall_ms":..,"io_ms":..,"compute_ms":..,
 *                  "cache_hit_pct":..,"delta_text":".."}
 *
 * The answer arrives as a delta: `delta_reasoning`/`delta_text` append to what this generation
 * already received, unless the line carries `"reset":1` — the engine's chat parser retroactively
 * reclassified answer text as reasoning (a closing tag arrived), and the deltas are full
 * snapshots that REPLACE the accumulated state. Sending the cumulative text every token made a
 * generation O(n²) on the wire (engine #119).
 *
 * The session control lines (BMOE_READY/BEGIN/DONE/ERROR) and the one-shot text summary lines
 * are handled by the RunService state machine, not here.
 */
class TelemetryParser {
    var current = Telemetry()
        private set

    // Accumulated answer/reasoning of the generation in flight. StringBuilder, not the data
    // class's String fields: appending a token to a String re-copies the whole answer per token,
    // which is the O(n²) this protocol change removes.
    private val text = StringBuilder()
    private val reasoning = StringBuilder()

    /** Clear the per-token state at the start of a new generation. */
    fun reset() {
        current = Telemetry()
        text.setLength(0)
        reasoning.setLength(0)
    }

    /** Returns true if [line] updated the token telemetry (UI should refresh). */
    fun onLine(line: String): Boolean {
        val t = line.trim()
        if (!t.startsWith("BMOE_PROGRESS ")) return false
        return runCatching {
            val o = JSONObject(t.removePrefix("BMOE_PROGRESS "))
            current.step = o.optInt("step")
            current.steps = o.optInt("steps")
            current.wallMs = o.optDouble("wall_ms")
            current.computeMs = o.optDouble("compute_ms")
            current.ioMs = o.optDouble("io_ms", 0.0)
            current.stallMs = o.optDouble("stall_ms", 0.0)
            current.mgmtMs = o.optDouble("mgmt_ms", 0.0)
            current.cacheHitPct = o.optDouble("cache_hit_pct", -1.0)
            current.majflt = o.optDouble("majflt", 0.0)
            current.cpuMs = o.optDouble("cpu_ms", 0.0)
            if (o.optInt("reset") == 1) {
                reasoning.setLength(0)
                text.setLength(0)
            }
            reasoning.append(o.optString("delta_reasoning"))
            text.append(o.optString("delta_text"))
            current.reasoning = reasoning.toString()
            current.text = text.toString()
        }.isSuccess
    }
}
