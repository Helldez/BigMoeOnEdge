package io.bigmoeonedge.example

import android.content.Intent
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.FileProvider
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.abs

/**
 * Reads back the per-token CSVs the engine wrote (`--csv`, see [AppSettings]): view one, share one,
 * or plot one column of two against each other.
 *
 * The compare view is the point. A claim like "the cache budget now falls under pressure" is not
 * settled by a number in a summary — it is settled by seeing the same column from a run with the
 * feature off and a run with it on, on one axis. Everything here reads columns by NAME, so a CSV
 * from an older build (fewer columns) still plots whatever it does have.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MetricsScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    var picked by remember { mutableStateOf<File?>(null) }
    var selection by remember { mutableStateOf<List<File>>(emptyList()) }
    var comparing by remember { mutableStateOf(false) }
    var refresh by remember { mutableStateOf(0) }
    val files = remember(refresh) {
        File(context.getExternalFilesDir(null), "metrics")
            .listFiles { f -> f.name.endsWith(".csv") }
            ?.sortedByDescending { it.lastModified() }
            ?: emptyList()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        when {
                            comparing -> "Compare"
                            picked != null -> picked!!.name
                            else -> "Metrics"
                        },
                        maxLines = 1,
                    )
                },
                navigationIcon = {
                    IconButton(onClick = {
                        when {
                            comparing -> comparing = false
                            picked != null -> picked = null
                            else -> onBack()
                        }
                    }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
                },
                actions = {
                    picked?.let { f -> TextButton(onClick = { shareCsv(context, listOf(f)) }) { Text("Share") } }
                },
            )
        },
        bottomBar = {
            // Only while picking: two files is what a comparison is.
            if (!comparing && picked == null && selection.isNotEmpty()) {
                BottomAppBar {
                    Spacer(Modifier.width(12.dp))
                    Text("${selection.size} selected", modifier = Modifier.weight(1f), fontSize = 13.sp)
                    TextButton(onClick = { shareCsv(context, selection) }) { Text("Share") }
                    TextButton(
                        onClick = { comparing = true },
                        enabled = selection.size == 2,
                    ) { Text("Compare") }
                    Spacer(Modifier.width(8.dp))
                }
            }
        },
    ) { padding ->
        val file = picked
        when {
            comparing -> CompareView(selection[0], selection[1], Modifier.padding(padding))
            file != null -> CsvView(file, Modifier.padding(padding))
            else -> FileList(
                files = files,
                selection = selection,
                modifier = Modifier.padding(padding),
                onPick = { picked = it },
                onToggle = { f ->
                    selection = when {
                        selection.contains(f) -> selection - f
                        selection.size < 2 -> selection + f
                        else -> listOf(selection[1], f) // a third pick replaces the oldest
                    }
                },
                onDelete = { f -> f.delete(); selection = selection - f; refresh++ },
            )
        }
    }
}

/** Hand the files to another app as content:// URIs (see the FileProvider in the manifest). */
private fun shareCsv(context: android.content.Context, files: List<File>) {
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

@Composable
private fun FileList(
    files: List<File>,
    selection: List<File>,
    modifier: Modifier,
    onPick: (File) -> Unit,
    onToggle: (File) -> Unit,
    onDelete: (File) -> Unit,
) {
    if (files.isEmpty()) {
        Column(modifier.fillMaxSize().padding(24.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("No metrics yet.", fontWeight = FontWeight.Medium)
            Text(
                "Turn on Settings → Diagnostics → Metrics CSV, then run a prompt. One file is written " +
                    "per session, covering every turn in it. Tick two to compare them.",
                fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        return
    }
    val stamp = SimpleDateFormat("d MMM HH:mm:ss", Locale.getDefault())
    LazyColumn(modifier.fillMaxSize()) {
        items(files) { f ->
            ListItem(
                leadingContent = {
                    Checkbox(checked = selection.contains(f), onCheckedChange = { onToggle(f) })
                },
                headlineContent = { Text(f.name, fontFamily = FontFamily.Monospace, fontSize = 13.sp) },
                supportingContent = {
                    Text("${stamp.format(Date(f.lastModified()))} · ${f.length() / 1024} KiB", fontSize = 12.sp)
                },
                trailingContent = { TextButton(onClick = { onDelete(f) }) { Text("Delete") } },
                modifier = Modifier.clickable(onClick = { onPick(f) }),
            )
            HorizontalDivider()
        }
    }
}

// ── the CSV, parsed once ────────────────────────────────────────────────────────────────────────

/** A parsed CSV: columns by name, plus the engine's per-turn `# summary` trailers. */
private class Csv(val header: List<String>, val rows: List<List<String>>, val summaries: List<String>) {
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
            val rows = lines.drop(1).filter { it.isNotBlank() && !it.startsWith("#") }.map { it.split(",") }
            return Csv(header, rows, lines.filter { it.startsWith("# summary") })
        }
    }
}

@Composable
private fun CsvView(file: File, modifier: Modifier) {
    // One row per token — thousands at worst, ~112 bytes each — so the whole file fits in memory
    // and there is nothing to gain from streaming it.
    val csv = remember(file) { Csv.read(file) }
    val hscroll = rememberScrollState()
    Column(modifier.fillMaxSize()) {
        csv.summaries.forEach { s ->
            Surface(color = MaterialTheme.colorScheme.surfaceVariant, modifier = Modifier.fillMaxWidth()) {
                Text(
                    s.removePrefix("# summary ").replace(" ", "   "),
                    fontFamily = FontFamily.Monospace, fontSize = 11.sp,
                    modifier = Modifier.padding(10.dp),
                )
            }
            Spacer(Modifier.height(4.dp))
        }
        Text(
            "${csv.rows.size} tokens · ${csv.header.size} columns · scroll sideways for all of them",
            fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
        )
        // One horizontal scroll shared by the header and every row, so the columns stay aligned.
        Column(Modifier.horizontalScroll(hscroll)) {
            Row(Modifier.padding(horizontal = 8.dp)) { csv.header.forEach { h -> Cell(h, bold = true) } }
            HorizontalDivider()
            LazyColumn(Modifier.fillMaxWidth()) {
                items(csv.rows) { r -> Row(Modifier.padding(horizontal = 8.dp)) { r.forEach { v -> Cell(v) } } }
            }
        }
    }
}

@Composable
private fun Cell(text: String, bold: Boolean = false) {
    Text(
        text,
        fontFamily = FontFamily.Monospace,
        fontSize = 11.sp,
        fontWeight = if (bold) FontWeight.Bold else FontWeight.Normal,
        maxLines = 1,
        modifier = Modifier.width(88.dp).padding(vertical = 3.dp, horizontal = 2.dp),
    )
}

// ── compare ─────────────────────────────────────────────────────────────────────────────────────

@Composable
private fun CompareView(fa: File, fb: File, modifier: Modifier) {
    val a = remember(fa) { Csv.read(fa) }
    val b = remember(fb) { Csv.read(fb) }
    // Only columns both files actually have: plotting a series against a blank is not a comparison.
    val metrics = remember(a, b) { a.plottable().filter { it in b.plottable() } }
    // Default to the column this whole feature was built to look at, if it is there.
    var metric by remember(metrics) {
        mutableStateOf(metrics.firstOrNull { it == "cache_budget_mib" } ?: metrics.firstOrNull() ?: "")
    }
    var expanded by remember { mutableStateOf(false) }

    val colorA = MaterialTheme.colorScheme.primary
    val colorB = MaterialTheme.colorScheme.error

    Column(modifier.fillMaxSize().padding(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        if (metrics.isEmpty()) {
            Text("These two files share no numeric column.", fontSize = 13.sp)
            return@Column
        }

        Box {
            OutlinedButton(onClick = { expanded = true }) { Text(metric.ifEmpty { "pick a column" }) }
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                metrics.forEach { m ->
                    DropdownMenuItem(
                        text = { Text(m, fontFamily = FontFamily.Monospace, fontSize = 13.sp) },
                        onClick = { metric = m; expanded = false },
                    )
                }
            }
        }

        val sa = a.column(metric).orEmpty()
        val sb = b.column(metric).orEmpty()
        LineChart(sa, sb, colorA, colorB, Modifier.fillMaxWidth().height(260.dp))

        Legend(fa.name, colorA, sa)
        Legend(fb.name, colorB, sb)
        Text(
            "x = token index within the file (turns run end to end). Both series share one y scale, " +
                "which is the only way the comparison means anything.",
            fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun Legend(name: String, color: Color, series: List<Float?>) {
    val vals = series.filterNotNull()
    Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Canvas(Modifier.size(14.dp)) { drawRect(color) }
        Column {
            Text(name, fontFamily = FontFamily.Monospace, fontSize = 11.sp, maxLines = 1)
            Text(
                if (vals.isEmpty()) "no data" else
                    "n=${vals.size}  min=${fmt(vals.min())}  mean=${fmt(vals.average().toFloat())}  max=${fmt(vals.max())}",
                fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

private fun fmt(v: Float): String = when {
    abs(v) >= 1000f -> String.format(Locale.US, "%.0f", v)
    abs(v) >= 10f -> String.format(Locale.US, "%.1f", v)
    else -> String.format(Locale.US, "%.2f", v)
}

/**
 * Two series on one pair of axes, drawn by hand rather than by pulling in a charting library for a
 * single plot. Both share one y range: two auto-scaled axes would make any two runs look alike,
 * which is the opposite of the job.
 */
@Composable
private fun LineChart(a: List<Float?>, b: List<Float?>, colorA: Color, colorB: Color, modifier: Modifier) {
    val grid = MaterialTheme.colorScheme.outlineVariant
    val all = (a + b).filterNotNull()
    if (all.isEmpty()) {
        Box(modifier) { Text("nothing to plot", fontSize = 12.sp) }
        return
    }
    var lo = all.min()
    var hi = all.max()
    if (hi - lo < 1e-6f) { hi += 1f; lo -= 1f } // a flat series is a fact, not a divide-by-zero
    val n = maxOf(a.size, b.size, 2)

    Canvas(modifier) {
        val w = size.width
        val h = size.height
        fun x(i: Int) = w * i / (n - 1).toFloat()
        fun y(v: Float) = h - (v - lo) / (hi - lo) * h

        // Three gridlines: enough to read a level off, few enough not to be a cage.
        listOf(0f, 0.5f, 1f).forEach { t ->
            val yy = h * t
            drawLine(grid, Offset(0f, yy), Offset(w, yy), strokeWidth = 1f)
        }

        fun draw(series: List<Float?>, color: Color) {
            val path = Path()
            var started = false
            series.forEachIndexed { i, v ->
                if (v == null) return@forEachIndexed // a gap is a gap: do not bridge it with a lie
                val px = x(i)
                val py = y(v)
                if (started) path.lineTo(px, py) else { path.moveTo(px, py); started = true }
            }
            if (started) drawPath(path, color, style = androidx.compose.ui.graphics.drawscope.Stroke(width = 3f))
        }
        draw(a, colorA)
        draw(b, colorB)
    }
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(fmt(lo), fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(fmt(hi), fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
