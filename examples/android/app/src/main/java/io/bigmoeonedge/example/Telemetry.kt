package io.bigmoeonedge.example

import org.json.JSONObject

/** Live per-token metrics parsed from bmoe-cli's --progress output. */
data class Telemetry(
    var step: Int = 0,
    var steps: Int = 0,
    var wallMs: Double = 0.0,
    var ioMs: Double = 0.0,
    var computeMs: Double = 0.0,
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
    // Aggregate decode rate over the whole run, parsed from the final summary line; -1 until
    // generation finishes. The per-token [tokensPerSecond] is instantaneous (last token only),
    // so the UI shows this average once it is available.
    var avgTokensPerSecond: Double = -1.0,
    // Per-token AVERAGES over the whole run, from the final summary — shown at the end instead of
    // the last token's instantaneous [computeMs]/[ioMs]. -1 until generation finishes.
    var avgComputeMs: Double = -1.0,
    var avgIoMs: Double = -1.0,
    var avgMgmtMs: Double = -1.0,
    // End-of-run figures from the final summary (BMOE_DONE); -1 / 0 until generation finishes.
    var prefillTps: Double = -1.0,      // prompt prefill rate (tok/s)
    var ttftS: Double = -1.0,           // time-to-first-token = model load + prompt prefill (s)
    var readMib: Double = -1.0,         // total flash streamed this generation (MiB)
    var cacheResidentMib: Double = -1.0, // expert cache resident size (MiB)
    var cacheBudgetMib: Double = -1.0,  // current (possibly auto-adapting) cache budget (MiB)
    // Run averages of the compute decomposition, from the final summary (BMOE_DONE); -1 until done.
    var avgMajfltPerTok: Double = -1.0, // major page faults per token over the run
    var avgCpuSPerTok: Double = -1.0,   // CPU-seconds per token (summed across threads) over the run
) {
    val tokensPerSecond: Double get() = if (wallMs > 0) 1000.0 / wallMs else 0.0
}

/**
 * Incrementally parses the CLI's per-token telemetry contract (see docs/telemetry.md):
 *   BMOE_LOAD     {"mb":..,"ms":..}
 *   BMOE_PROGRESS {"step":..,"steps":..,"wall_ms":..,"io_ms":..,"compute_ms":..,
 *                  "cache_hit_pct":..,"text":".."}
 *
 * The session control lines (BMOE_READY/BEGIN/DONE/ERROR) and the one-shot text summary lines
 * are handled by the RunService state machine, not here.
 */
class TelemetryParser {
    var current = Telemetry()
        private set

    /** Clear the per-token state at the start of a new generation. */
    fun reset() {
        current = Telemetry()
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
            current.ioMs = o.optDouble("io_ms")
            current.computeMs = o.optDouble("compute_ms")
            current.mgmtMs = o.optDouble("mgmt_ms", 0.0)
            current.cacheHitPct = o.optDouble("cache_hit_pct", -1.0)
            current.majflt = o.optDouble("majflt", 0.0)
            current.cpuMs = o.optDouble("cpu_ms", 0.0)
            current.text = o.optString("text")
        }.isSuccess
    }
}
