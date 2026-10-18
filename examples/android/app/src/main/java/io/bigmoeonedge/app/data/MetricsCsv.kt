package io.bigmoeonedge.app.data

import android.content.Context
import android.content.Intent
import androidx.core.content.FileProvider
import io.bigmoeonedge.app.engine.ParamSpec
import java.io.File
import java.util.Locale
import kotlin.math.abs
import kotlin.math.sqrt

/**
 * The data layer behind [MetricsScreen]: reading the engine's per-token CSVs, naming a run, the
 * arithmetic the charts rest on, and handing a file to another app.
 *
 * None of this draws anything, and that is the point of the split — [MetricsScreen] is five views
 * over this, and the two used to live in one file where the parser and the Canvas code sat a scroll
 * apart. Everything reads columns by NAME, so a CSV from an older build (fewer columns) still plots
 * whatever it does have.
 */

/**
 * A parsed CSV: columns by name, the engine's `# bmoe_metrics` preamble (what the run WAS), and its
 * per-turn `# summary` trailers (what each turn DID).
 */
internal class Csv(
    val header: List<String>,
    val rows: List<List<String>>,
    val summaries: List<String>,
    val info: Map<String, String>,
) {
    /**
     * The run's identity, short enough to sit under a chart line: the model, whether it streamed,
     * and every setting that changes the reply and differs from its default. Those belong in the
     * identity, not only in the config table: a run that dropped experts is not the same KIND of run
     * as one that did not, and two compare legends that differ only by one used to read as identical
     * (#136). Which settings change the reply is the parameter table's to say. Empty for a
     * pre-preamble CSV.
     */
    fun label(glossary: ConfigGlossary): String {
        if (info.isEmpty()) return ""
        val streamed = (info[PreambleFacts.STREAMING] ?: info[PreambleFacts.STREAMING_BEFORE])
            ?.let { if (it == "1") "streamed" else "mmap baseline" }
        val lossy = glossary.lossyChanged(info).map { (spec, v) -> "${spec.label} ${glossary.value(spec.key, v)}" }
        return (listOfNotNull(info[PreambleFacts.MODEL], streamed) + lossy).joinToString(" · ")
    }

    /**
     * This column's values, or null where the column is absent, the cell is not a number, or the
     * cell is a documented "not sampled" sentinel. `cache_hit_pct` and `dense_resident_frac` write
     * -1 when the engine had nothing to report (before the first sample, streaming off, /proc
     * unreadable); that is missing data, not a measurement. Mapping it to null — rather than
     * dropping the row — keeps every column index-aligned, so the compare view's min/mean/max and
     * pearson() (which pairs by position) stop counting -1 as a real value. Genuine -1s in other
     * columns are left untouched.
     */
    fun column(name: String): List<Float?>? {
        val i = header.indexOf(name).takeIf { it >= 0 } ?: return null
        val sentinel = name in NOT_SAMPLED_MINUS_ONE
        return rows.map { row -> row.getOrNull(i)?.toFloatOrNull()?.takeUnless { sentinel && it == -1f } }
    }

    /** Columns worth plotting: numeric, and not an axis or a label. */
    fun plottable(): List<String> =
        header.filter { it !in setOf("step", "steps", "turn") && column(it)?.any { v -> v != null } == true }

    companion object {
        // Columns where -1 means "not sampled", not a value. See column().
        private val NOT_SAMPLED_MINUS_ONE = setOf("cache_hit_pct", "dense_resident_frac")

        fun read(f: File): Csv {
            val lines = runCatching { f.readLines() }.getOrElse { emptyList() }
            val header = lines.firstOrNull { !it.startsWith("#") }?.split(",") ?: emptyList()
            val rows = lines.filter { it.isNotBlank() && !it.startsWith("#") }.drop(1).map { it.split(",") }
            // The preamble's key=value pairs, order-independent, unknown keys kept: same contract
            // as the `# summary` trailer, so an older or newer engine still parses.
            val info = lines.filter { it.startsWith("# ") && !it.startsWith("# summary") }
                .flatMap { it.removePrefix("# ").trim().split(" ") }
                .mapNotNull { tok -> tok.split("=", limit = 2).takeIf { it.size == 2 } }
                .associate { it[0] to it[1] }
            return Csv(header, rows, lines.filter { it.startsWith("# summary") }, info)
        }
    }
}

/**
 * A short human name for a run: the model's initial and each turn's tok/s, e.g. "Q · 1.45→0.75 t/s"
 * for a two-turn Qwen session. Reads only the file's `#` lines. Empty when the file has no header
 * (a pre-preamble CSV), so the caller falls back to the filename.
 */
internal fun runTitle(file: File): String {
    val lines = runCatching { file.readLines() }.getOrElse { return "" }
    // Find the `model=` token wherever it sits in the preamble, rather than requiring the line to
    // begin with it: the preamble grows by appending keys and lines, and a title should not be the
    // one thing that depends on their order.
    val model = lines.filter { it.startsWith("# ") && !it.startsWith("# summary") }
        .flatMap { it.removePrefix("# ").trim().split(" ") }
        .firstOrNull { it.startsWith("model=") }
        ?.substringAfter("model=", "").orEmpty()
    if (model.isEmpty()) return ""
    val initial = model.first().uppercaseChar()
    val tps = lines.filter { it.startsWith("# summary") }
        .mapNotNull { summaryFields(it)["tok/s"]?.toFloatOrNull() }
    val tpsStr = if (tps.isEmpty()) "" else tps.joinToString("→") { fmt(it) } + " t/s"
    return listOf(initial.toString(), tpsStr).filter { it.isNotEmpty() }.joinToString(" · ")
}

/**
 * One `# summary` line's fields: whitespace-separated `key=value`, order-independent, the same
 * contract as the preamble, so a field is read by its name and a new one breaks nothing.
 */
internal fun summaryFields(line: String): Map<String, String> =
    line.removePrefix("#").trim().removePrefix("summary").trim().split(' ', '\t')
        .mapNotNull { tok -> tok.split("=", limit = 2).takeIf { it.size == 2 && it[0].isNotEmpty() } }
        .associate { it[0] to it[1] }

/**
 * The preamble's settings, explained by the parameter table (`GET /api/params`): a setting is
 * written under its parameter's key with `_` for `-`, so its label, its type, its unit, its help
 * and whether it changes the reply are the table's, never a copy kept here. A key the table does
 * not know (a fact the app wrote from `/api/info`, or a setting of another build) is shown under
 * its own name: unexplained, which is the right failure, and not a reason to hide it.
 */
internal class ConfigGlossary(val params: List<ParamSpec>) {
    private val byKey = params.associateBy { preambleKey(it.key) }

    fun spec(key: String): ParamSpec? = byKey[key]
    fun label(key: String): String = spec(key)?.label ?: PreambleFacts.label(key) ?: key

    /** A preamble value as the table types it: a switch as on/off, a number with its unit. */
    fun value(key: String, v: String): String {
        val s = spec(key) ?: return v
        return when {
            s.type == "bool" -> if (v == "1" || v == "true") "on" else "off"
            s.unit != null && v.toDoubleOrNull() != null -> "$v ${s.unit}"
            else -> v
        }
    }

    /** Every row of a preamble as (key, label, value): the facts first, then the table's order, then the rest. */
    fun rows(info: Map<String, String>): List<Triple<String, String, String>> {
        val facts = info.keys.filter { PreambleFacts.label(it) != null && spec(it) == null }
        val settings = params.map { preambleKey(it.key) }.filter { it in info }
        val rest = info.keys.filter { it !in facts && it !in settings }.sorted()
        return (facts + settings + rest).map { k -> Triple(k, label(k), value(k, info.getValue(k))) }
    }

    /** The settings of a run that change the reply and are not at their default, in table order. */
    fun lossyChanged(info: Map<String, String>): List<Pair<ParamSpec, String>> =
        params.filter { it.lossy }.mapNotNull { s ->
            val v = info[preambleKey(s.key)] ?: return@mapNotNull null
            if (sameValue(s, v, s.default)) null else s to v
        }

    private fun sameValue(s: ParamSpec, v: String, default: Any?): Boolean = when (default) {
        null -> false
        is Boolean -> (v == "1" || v == "true") == default
        is Number -> v.toDoubleOrNull() == default.toDouble()
        else -> v == default.toString()
    }

    companion object {
        /** How the recorder writes a parameter's key in the preamble. */
        fun preambleKey(paramKey: String): String = paramKey.replace('-', '_')
    }
}

/** Enough significant digits to tell two runs apart, without a column of noise. */
internal fun fmt(v: Float): String = when {
    abs(v) >= 1000f -> String.format(Locale.US, "%.0f", v)
    abs(v) >= 10f -> String.format(Locale.US, "%.1f", v)
    else -> String.format(Locale.US, "%.2f", v)
}

/** Pearson correlation over the positions where BOTH series have a value; null if too few. */
internal fun pearson(a: List<Float?>, b: List<Float?>): Float? {
    val pairs = a.zip(b).mapNotNull { (x, y) -> if (x != null && y != null) x to y else null }
    if (pairs.size < 3) return null
    val n = pairs.size
    val mx = pairs.sumOf { it.first.toDouble() } / n
    val my = pairs.sumOf { it.second.toDouble() } / n
    var sxy = 0.0; var sxx = 0.0; var syy = 0.0
    for ((x, y) in pairs) {
        val dx = x - mx; val dy = y - my
        sxy += dx * dy; sxx += dx * dx; syy += dy * dy
    }
    if (sxx <= 0.0 || syy <= 0.0) return null // a flat series correlates with nothing
    return (sxy / sqrt(sxx * syy)).toFloat()
}

/** Hand the files to another app as content:// URIs (see the FileProvider in the manifest). */
internal fun shareCsv(context: Context, files: List<File>) {
    val uris = ArrayList(files.map {
        FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", it)
    })
    val intent = Intent(if (uris.size == 1) Intent.ACTION_SEND else Intent.ACTION_SEND_MULTIPLE).apply {
        type = "text/csv"
        if (uris.size == 1) putExtra(Intent.EXTRA_STREAM, uris[0]) else putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris)
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
    runCatching { context.startActivity(Intent.createChooser(intent, "Share metrics").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
}
