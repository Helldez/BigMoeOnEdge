package io.bigmoeonedge.app.data

import android.content.Context
import io.bigmoeonedge.app.engine.ConfigState
import io.bigmoeonedge.app.engine.ServerInfo
import org.json.JSONObject
import java.io.File
import java.io.FileWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Writes one per-token CSV per loaded session from the server's `token` and `done` events, in the
 * shape the engine's own `--csv` sink writes and [MetricsScreen] reads: a `#` preamble with the
 * configuration the run was opened with, one row per token with the event's columns, and a
 * `# summary` line per turn. The engine's sink is a launch-time file of the server process, which
 * outlives many sessions; writing here keeps one file per model loaded, which is the unit a
 * comparison is made of.
 *
 * Files land under the app's external files dir, `metrics/bmoe-<timestamp>.csv`: readable by the
 * Metrics screen, shareable through the FileProvider, and `adb pull`-able without root.
 */
class MetricsRecorder(private val ctx: Context) {
    private var writer: FileWriter? = null
    private var turn = 0
    private var headerWritten = false

    @Synchronized
    fun sessionOpened(info: ServerInfo, config: ConfigState?) {
        close()
        if (!AppPrefs.metricsCsv(ctx)) return
        val dir = File(ctx.getExternalFilesDir(null) ?: return, "metrics")
        if (!dir.isDirectory && !dir.mkdirs()) return
        val ts = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val f = File(dir, "bmoe-$ts.csv")
        writer = runCatching { FileWriter(f) }.getOrNull() ?: return
        turn = 0
        headerWritten = false
        val model = info.model
        // The first line is what the server said about the session (PreambleFacts); the second is
        // every configuration value it reports, under the parameter's key with `_` for `-`, so a
        // file can state what it was run with and the parameter table can explain it.
        val head = StringBuilder("# bmoe_metrics v=app engine=").append(info.version)
        if (model != null) {
            head.append(" ${PreambleFacts.MODEL}=").append(model.name.replace(' ', '_'))
                .append(" arch=").append(model.arch)
                .append(" n_ctx=").append(model.nCtx)
                .append(" n_expert_used=").append(model.nExpertUsed)
                .append(" ${PreambleFacts.STREAMING}=").append(if (model.streaming) 1 else 0)
                .append(" compute_threads=").append(model.computeThreads)
                .append(" busy_threads=").append(model.busyThreads)
        }
        write(head.toString() + "\n")
        if (config != null) {
            val line = StringBuilder("#")
            for ((k, v) in config.values.toSortedMap()) {
                if (k == "model" || v == null) continue
                val value = when (v) {
                    is Boolean -> if (v) "1" else "0"
                    else -> v.toString().replace(' ', '_')
                }
                line.append(' ').append(ConfigGlossary.preambleKey(k)).append('=').append(value)
            }
            write(line.toString() + "\n")
        }
    }

    @Synchronized
    fun token(o: JSONObject) {
        val w = writer ?: return
        if (!headerWritten) {
            write(COLUMNS.joinToString(",") + ",turn\n")
            headerWritten = true
            turn++
        } else if (o.optInt("step") == 1) {
            turn++
        }
        val row = COLUMNS.joinToString(",") { c -> cell(o.opt(c)) } + "," + turn
        runCatching { w.write(row + "\n") }
    }

    @Synchronized
    fun done(o: JSONObject) {
        val w = writer ?: return
        val loc = Locale.US
        val line = buildString {
            append("# summary turn=").append(turn)
            append(" tokens=").append(o.optInt("tokens"))
            append(String.format(loc, " tok/s=%.3f", o.optDouble("tok_s", 0.0)))
            append(String.format(loc, " prefill_s=%.3f", o.optDouble("prefill_s", 0.0)))
            append(" n_prompt=").append(o.optInt("n_prompt"))
            append(" n_past=").append(o.optInt("n_past"))
            append(String.format(loc, " cache_hit_pct=%.1f", o.optDouble("cache_hit_pct", -1.0)))
            append(String.format(loc, " read_mib=%.1f", o.optDouble("read_mib", 0.0)))
            append(String.format(loc, " majflt_tok=%.2f", o.optDouble("majflt_tok", 0.0)))
            append(String.format(loc, " cpu_s_tok=%.4f", o.optDouble("cpu_s_tok", 0.0)))
            if (o.optBoolean("cancelled")) append(" cancelled=1")
        }
        runCatching { w.write(line + "\n"); w.flush() }
    }

    @Synchronized
    fun close() {
        runCatching { writer?.close() }
        writer = null
    }

    private fun write(s: String) {
        runCatching { writer?.write(s) }
    }

    private fun cell(v: Any?): String = when (v) {
        null -> ""
        is Double -> if (v == Math.floor(v) && !v.isInfinite() && Math.abs(v) < 1e15) v.toLong().toString()
        else String.format(Locale.US, "%.4f", v)
        else -> v.toString()
    }

    companion object {
        /** The token event's columns, in the engine sink's order where the two share a name. */
        val COLUMNS = listOf(
            "step", "steps", "wall_ms", "io_ms", "compute_ms", "mgmt_ms", "stall_ms", "flash_wait_ms", "read_mib",
            "cache_hit_pct", "cache_budget_mib", "rss_mib", "mem_available_mib", "majflt", "cpu_ms",
            "dense_resident_frac", "mtp_batch",
        )
    }
}
