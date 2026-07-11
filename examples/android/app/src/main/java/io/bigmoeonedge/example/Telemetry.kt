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
 * Incrementally parses the CLI's telemetry contract (see docs/telemetry.md):
 *   BMOE_LOAD     {"mb":..,"ms":..}
 *   BMOE_PROGRESS {"step":..,"steps":..,"wall_ms":..,"io_ms":..,"compute_ms":..,
 *                  "cache_hit_pct":..,"text":".."}
 * plus the trailing summary lines (generation:/moe-stream:/moe-cache:).
 */
class TelemetryParser {
    val current = Telemetry()
    var summary: String = ""
        private set

    /** Returns true if [line] updated the token telemetry (UI should refresh). */
    fun onLine(line: String): Boolean {
        val t = line.trim()
        return when {
            t.startsWith("BMOE_PROGRESS ") -> {
                runCatching {
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
            t.startsWith("generation:") || t.startsWith("moe-stream:") || t.startsWith("moe-cache:") -> {
                summary = if (summary.isEmpty()) t else summary + "\n" + t
                // "generation: N tokens, X s/token (Y tok/s)" — Y is the aggregate average.
                if (t.startsWith("generation:")) {
                    Regex("""\(([\d.]+) tok/s\)""").find(t)?.groupValues?.get(1)?.toDoubleOrNull()
                        ?.let { current.avgTokensPerSecond = it }
                }
                true
            }
            else -> false
        }
    }
}
