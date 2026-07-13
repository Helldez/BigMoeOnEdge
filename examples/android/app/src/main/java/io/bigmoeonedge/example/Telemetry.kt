package io.bigmoeonedge.example

import org.json.JSONObject

/** Live per-token metrics parsed from bmoe-cli's --progress output. */
data class Telemetry(
    var step: Int = 0,
    var steps: Int = 0,
    var wallMs: Double = 0.0,
    var ioMs: Double = 0.0,
    var computeMs: Double = 0.0,
    var cacheHitPct: Double = -1.0,
    var text: String = "",
    // Aggregate decode rate over the whole run, parsed from the final summary line; -1 until
    // generation finishes. The per-token [tokensPerSecond] is instantaneous (last token only),
    // so the UI shows this average once it is available.
    var avgTokensPerSecond: Double = -1.0,
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
            current.cacheHitPct = o.optDouble("cache_hit_pct", -1.0)
            current.text = o.optString("text")
        }.isSuccess
    }
}
