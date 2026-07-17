package io.bigmoeonedge.example

import android.content.Context
import android.content.Intent
import androidx.core.content.FileProvider
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
    /** The run's identity, short enough to sit under a chart line. Empty for a pre-preamble CSV. */
    fun label(): String {
        if (info.isEmpty()) return ""
        val cache = when {
            info["cache_auto"] == "1" -> "cache auto(${info["cache_mb"]})"
            else -> "cache ${info["cache_mb"]}"
        }
        return listOfNotNull(
            info["model"],
            info["n_expert_used"]?.let { "top-$it" },
            if (info["moe_stream"] == "1") cache else "mmap baseline",
            info["io_threads"]?.let { "$it lanes" },
            if (info["overlap"] == "1") "overlap" else null,
            info["prefetch"]?.takeIf { it != "0" }?.let { "prefetch $it" },
        ).joinToString(" · ")
    }

    /** This column's values, or null where the column is absent or the cell is not a number. */
    fun column(name: String): List<Float?>? {
        val i = header.indexOf(name).takeIf { it >= 0 } ?: return null
        return rows.map { it.getOrNull(i)?.toFloatOrNull() }
    }

    /** Columns worth plotting: numeric, and not an axis or a label. */
    fun plottable(): List<String> =
        header.filter { it !in setOf("step", "steps", "turn") && column(it)?.any { v -> v != null } == true }

    companion object {
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
    val model = lines.firstOrNull { it.startsWith("# model=") }
        ?.substringAfter("model=", "")?.substringBefore(" ").orEmpty()
    if (model.isEmpty()) return ""
    val initial = model.first().uppercaseChar()
    val tps = lines.filter { it.startsWith("# summary") }
        .mapNotNull { Regex("""tok/s=([0-9.]+)""").find(it)?.groupValues?.get(1)?.toFloatOrNull() }
    val tpsStr = if (tps.isEmpty()) "" else tps.joinToString("→") { fmt(it) } + " t/s"
    return listOf(initial.toString(), tpsStr).filter { it.isNotEmpty() }.joinToString(" · ")
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
