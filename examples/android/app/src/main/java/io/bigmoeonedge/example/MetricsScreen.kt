package io.bigmoeonedge.example

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.abs

/**
 * Reads back the per-token CSVs the engine wrote (`--csv`, see [AppSettings]): view one, share one,
 * or plot one column of two against each other.
 *
 * The compare view is the point. A claim like "this setting cuts the fault storm" is not settled by
 * a number in a summary — it is settled by seeing the same column from a run with the feature off
 * and a run with it on, on one axis.
 *
 * This file is the views. Reading and parsing the CSVs, naming a run, the chart arithmetic and the
 * share intent live in [MetricsCsv.kt] — nothing here touches a file.
 */
// What the screen is showing. picked = view one file; correlating = overlay one file's own columns;
// comparing = two files' same column; glossary = the field reference.
private enum class View { LIST, FILE, CORRELATE, COMPARE, GLOSSARY }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MetricsScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    var picked by remember { mutableStateOf<File?>(null) }
    var selection by remember { mutableStateOf<List<File>>(emptyList()) }
    var view by remember { mutableStateOf(View.LIST) }
    var refresh by remember { mutableStateOf(0) }
    val files = remember(refresh) {
        File(context.getExternalFilesDir(null), "metrics")
            .listFiles { f -> f.name.endsWith(".csv") }
            ?.sortedByDescending { it.lastModified() }
            ?: emptyList()
    }

    // Back always steps one level toward the list, then out of the screen.
    fun back() {
        when (view) {
            View.CORRELATE -> view = View.FILE
            View.FILE, View.COMPARE, View.GLOSSARY -> view = View.LIST
            View.LIST -> onBack()
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        when (view) {
                            View.COMPARE -> "Compare files"
                            View.CORRELATE -> "Correlate fields"
                            View.GLOSSARY -> "What the columns mean"
                            View.FILE -> picked?.name ?: "Metrics"
                            View.LIST -> "Metrics"
                        },
                        maxLines = 1,
                    )
                },
                navigationIcon = {
                    IconButton(onClick = { back() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    if (view == View.FILE) {
                        TextButton(onClick = { view = View.CORRELATE }) { Text("Correlate") }
                        picked?.let { f -> TextButton(onClick = { shareCsv(context, listOf(f)) }) { Text("Share") } }
                    }
                    if (view == View.LIST || view == View.FILE) {
                        TextButton(onClick = { view = View.GLOSSARY }) { Text("?") }
                    }
                },
            )
        },
        bottomBar = {
            if (view == View.LIST && selection.isNotEmpty()) {
                BottomAppBar {
                    Spacer(Modifier.width(12.dp))
                    Text("${selection.size} selected", modifier = Modifier.weight(1f), fontSize = 13.sp)
                    TextButton(onClick = { shareCsv(context, selection) }) { Text("Share") }
                    TextButton(
                        onClick = { view = View.COMPARE },
                        enabled = selection.size == 2,
                    ) { Text("Compare") }
                    Spacer(Modifier.width(8.dp))
                }
            }
        },
    ) { padding ->
        val m = Modifier.padding(padding)
        when (view) {
            View.COMPARE -> CompareView(selection[0], selection[1], m)
            View.CORRELATE -> picked?.let { CorrelateView(it, m) }
            View.GLOSSARY -> GlossaryView(m)
            View.FILE -> picked?.let { CsvView(it, m) }
            View.LIST -> FileList(
                files = files,
                selection = selection,
                modifier = m,
                onPick = { picked = it; view = View.FILE },
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
    // A run's identity, not its filename: the model's initial and each turn's tok/s, read from the
    // preamble and summaries. Computed once per file list — the files are small and few.
    val titles = remember(files) { files.associateWith { runTitle(it) } }
    LazyColumn(modifier.fillMaxSize()) {
        items(files) { f ->
            val title = titles[f].orEmpty()
            ListItem(
                leadingContent = {
                    Checkbox(checked = selection.contains(f), onCheckedChange = { onToggle(f) })
                },
                headlineContent = {
                    Text(title.ifEmpty { f.name }, fontWeight = FontWeight.Medium, fontSize = 15.sp)
                },
                supportingContent = {
                    // Timestamp + size, then the bare filename (the ID is now the headline). The
                    // filename drops its fixed "bmoe-" prefix and ".csv" tail — it is only there to
                    // match a shared file, and the stamp already carries the date.
                    val short = f.name.removePrefix("bmoe-").removeSuffix(".csv")
                    Text(
                        "${stamp.format(Date(f.lastModified()))} · ${f.length() / 1024} KiB · $short",
                        fontSize = 11.sp, maxLines = 1,
                    )
                },
                trailingContent = { TextButton(onClick = { onDelete(f) }) { Text("Delete") } },
                modifier = Modifier.clickable(onClick = { onPick(f) }),
            )
            HorizontalDivider()
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
        // What the run was, before what it did. A file whose configuration is unknown is a file
        // whose numbers cannot be compared to anything.
        if (csv.info.isNotEmpty()) {
            Surface(color = MaterialTheme.colorScheme.primaryContainer, modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(csv.label(), fontSize = 12.sp, fontWeight = FontWeight.Medium)
                    Text(
                        listOfNotNull(
                            csv.info["arch"]?.let { "arch $it" },
                            csv.info["n_layer"]?.let { "$it layers" },
                            csv.info["n_expert"]?.let { "$it experts" },
                            csv.info["threads"]?.let { "$it threads" },
                            csv.info["n_ctx"]?.let { "ctx $it" },
                            if (csv.info["o_direct"] == "1") "O_DIRECT" else null,
                            csv.info["dense_weights"]?.takeIf { csv.info["moe_stream"] == "1" }?.let { "dense=$it" },
                            if (csv.info["force_cache"] == "1") "forced" else null,
                        ).joinToString(" · "),
                        fontSize = 11.sp,
                    )
                }
            }
            Spacer(Modifier.height(4.dp))
        }
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
    // Open on a column that actually moves. This used to default to cache_budget_mib, back when a
    // governor made it move under pressure; the budget is fixed for a run now, so that default
    // opened the view on a flat line. The hit rate is what a config change shows up in first.
    var metric by remember(metrics) {
        mutableStateOf(metrics.firstOrNull { it == "cache_hit_pct" } ?: metrics.firstOrNull() ?: "")
    }
    var expanded by remember { mutableStateOf(false) }

    val colorA = MaterialTheme.colorScheme.primary
    val colorB = MaterialTheme.colorScheme.error

    if (metrics.isEmpty()) {
        Box(modifier.fillMaxSize().padding(12.dp)) { Text("These two files share no numeric column.", fontSize = 13.sp) }
        return
    }

    // The content is taller than the screen (chart + legends + two full configs), so it scrolls.
    val changed = remember(a, b) { (a.info.keys + b.info.keys).filter { a.info[it] != b.info[it] }.toSet() }
    Column(
        modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
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
        FieldNote(metric)

        val sa = a.column(metric).orEmpty()
        val sb = b.column(metric).orEmpty()
        LineChart(sa, sb, colorA, colorB, Modifier.fillMaxWidth().height(240.dp))

        Legend(fa.name, a.label(), colorA, sa)
        Legend(fb.name, b.label(), colorB, sb)
        Text(
            "x = token index within the file (turns run end to end). Both series share one y scale, " +
                "which is the only way the comparison means anything.",
            fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        // The full configuration of each run, changed settings highlighted — not just the diff, so a
        // setting that stayed the same (was dense=warm in both?) is still there to read.
        Text("Configuration", fontSize = 13.sp, fontWeight = FontWeight.Bold)
        FullConfig(a, colorA, changed)
        FullConfig(b, colorB, changed)
    }
}

@Composable
private fun Legend(name: String, label: String, color: Color, series: List<Float?>) {
    val vals = series.filterNotNull()
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Canvas(Modifier.size(12.dp).padding(top = 3.dp)) { drawRect(color) }
        Column {
            // The configuration first, the file name second: which run this is matters more than
            // what it happens to be called.
            if (label.isNotEmpty()) Text(label, fontSize = 11.sp, fontWeight = FontWeight.Medium)
            Text(name, fontFamily = FontFamily.Monospace, fontSize = 10.sp, maxLines = 1,
                 color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(
                if (vals.isEmpty()) "no data" else
                    "n=${vals.size}  min=${fmt(vals.min())}  mean=${fmt(vals.average().toFloat())}  max=${fmt(vals.max())}",
                fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

// The run preamble, every field, in the settings screen's order. Kept in full — not just what
// differs — because a setting that did NOT change between two runs is still worth seeing: "was warm
// dense on in both?" is a real question, and a diff that hides it cannot answer it.
private val CONFIG_ORDER = listOf(
    "model" to "Model",
    "arch" to "Architecture",
    "n_layer" to "Layers",
    "n_expert" to "Experts",
    "n_expert_used" to "Active experts (top-k)",
    "n_ctx" to "Context",
    "threads" to "Compute threads",
    "moe_stream" to "MoE streaming",
    "cache_mb" to "Cache budget (MiB)",
    "cache_auto" to "Cache auto-size",
    "cache_ceil_mb" to "Cache ceiling (MiB)",
    "force_cache" to "Force cache",
    "io_threads" to "I/O lanes",
    "o_direct" to "Direct I/O",
    "overlap" to "I/O–compute overlap",
    "prefetch" to "Temporal prefetch",
    "dense_weights" to "Dense weights",
)
private val CONFIG_BOOLS = setOf("moe_stream", "cache_auto", "force_cache", "o_direct", "overlap")

private fun prettyConfigValue(key: String, v: String): String = when {
    key in CONFIG_BOOLS -> if (v == "1") "on" else "off"
    key == "prefetch" && v == "0" -> "off"
    else -> v
}

/**
 * The complete run configuration of one file — every field from the preamble, present or absent.
 * `changed` marks the settings that differ from the other file, so "what changed" is still obvious
 * without hiding "what did not".
 */
@Composable
private fun FullConfig(csv: Csv, accent: Color, changed: Set<String>) {
    if (csv.info.isEmpty()) {
        Text("No run header in this file — its configuration is unknown.",
             fontSize = 11.sp, color = MaterialTheme.colorScheme.error)
        return
    }
    Surface(color = MaterialTheme.colorScheme.surfaceVariant, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                Canvas(Modifier.size(12.dp)) { drawRect(accent) }
                Text(csv.info["model"] ?: "run", fontSize = 12.sp, fontWeight = FontWeight.Bold, maxLines = 1)
            }
            CONFIG_ORDER.forEach { (key, label) ->
                val v = csv.info[key] ?: return@forEach // a field an older engine did not write
                val isChanged = key in changed
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text("$label:", fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                         modifier = Modifier.weight(1f))
                    Text(
                        prettyConfigValue(key, v),
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace,
                        // A changed setting is the reason to be here; make it the thing the eye lands on.
                        fontWeight = if (isChanged) FontWeight.Bold else FontWeight.Normal,
                        color = if (isChanged) accent else MaterialTheme.colorScheme.onSurface,
                    )
                }
            }
        }
    }
}

/**
 * One series as a stroked path, mapped into the y range [lo]..[hi] over [n] x slots.
 *
 * Where that range comes from is the entire difference between the two charts here — one shared
 * range for the two-file compare, a per-series range for the correlate overlay — so it stays with
 * the caller. What they share is this: a null is a gap, and a gap is never bridged, because a line
 * interpolated across missing data is a lie about data that isn't there.
 */
private fun DrawScope.drawSeries(values: List<Float?>, n: Int, lo: Float, hi: Float, color: Color) {
    val span = (hi - lo).takeIf { it > 1e-6f } ?: 1f
    val path = Path()
    var started = false
    values.forEachIndexed { i, v ->
        if (v == null) return@forEachIndexed
        val px = size.width * i / (n - 1).toFloat()
        val py = size.height - (v - lo) / span * size.height
        if (started) path.lineTo(px, py) else { path.moveTo(px, py); started = true }
    }
    if (started) drawPath(path, color, style = Stroke(width = 3f))
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
        // Three gridlines: enough to read a level off, few enough not to be a cage.
        listOf(0f, 0.5f, 1f).forEach { t ->
            val yy = size.height * t
            drawLine(grid, Offset(0f, yy), Offset(size.width, yy), strokeWidth = 1f)
        }
        // One range for both — that shared scale IS the comparison.
        drawSeries(a, n, lo, hi, colorA)
        drawSeries(b, n, lo, hi, colorB)
    }
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(fmt(lo), fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(fmt(hi), fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

// ── the field reference ─────────────────────────────────────────────────────────────────────────

/** "lower is better" / "higher is better" / "depends", coloured, in three characters plus a word. */
@Composable
private fun BetterBadge(better: Better) {
    val (text, color) = when (better) {
        Better.LOWER -> "↓ lower better" to MaterialTheme.colorScheme.primary
        Better.HIGHER -> "↑ higher better" to MaterialTheme.colorScheme.primary
        Better.NEUTRAL -> "– depends" to MaterialTheme.colorScheme.onSurfaceVariant
    }
    Text(text, fontSize = 11.sp, fontWeight = FontWeight.Medium, color = color)
}

/** The one-line meaning of the currently plotted column, under a chart or a picker. */
@Composable
private fun FieldNote(name: String) {
    val f = MetricFields.of(name) ?: return
    Column(verticalArrangement = Arrangement.spacedBy(1.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(f.short, fontSize = 12.sp, fontWeight = FontWeight.Medium)
            BetterBadge(f.better)
        }
        Text(f.measures, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun GlossaryView(modifier: Modifier) {
    LazyColumn(modifier.fillMaxSize().padding(horizontal = 12.dp)) {
        item {
            Text(
                "Every column the metrics CSV can hold. \"depends\" means neither direction is simply " +
                    "good — a bigger cache buys hits but risks the reclaim war, and mem_available lies.",
                fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 10.dp),
            )
        }
        items(MetricFields.all) { f ->
            Column(Modifier.padding(vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(f.name, fontFamily = FontFamily.Monospace, fontSize = 13.sp, fontWeight = FontWeight.Bold)
                    BetterBadge(f.better)
                }
                Text(f.short, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                Text(f.measures, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            HorizontalDivider()
        }
    }
}

// ── correlate: one file's own columns, normalized, on one axis ──────────────────────────────────

/**
 * Overlays several of a file's columns after squashing each to 0..1, so their SHAPES can be
 * compared even though their units cannot. The question it answers is "when this rises, does that
 * rise too" — e.g. does rss_anon fall exactly as majflt spikes, which says the faults are the cache
 * being taken and not the model. A Pearson r against the first-picked column puts a number on it.
 *
 * Normalization is per-column min..max: it discards magnitude on purpose. A flat line means the
 * column did not vary, not that it was zero — the legend keeps the real min/max so nothing is lost.
 */
@Composable
private fun CorrelateView(file: File, modifier: Modifier) {
    val csv = remember(file) { Csv.read(file) }
    val cols = remember(csv) { csv.plottable() }
    // Start on the memory war: what the kernel took back (rss_anon_mib), what that cost in re-reads
    // (majflt_mib), and how much of the dense set survived (dense_resident_frac) — which is what
    // tells "the faults are the model" apart from "the faults are the cache". The third used to be
    // cache_budget_mib, back when a governor moved it under pressure; it is fixed for the run now,
    // so it opened this view on a flat line.
    var chosen by remember(cols) {
        mutableStateOf(
            cols.filter { it in setOf("majflt_mib", "rss_anon_mib", "dense_resident_frac") }.ifEmpty { cols.take(2) }
        )
    }
    var expanded by remember { mutableStateOf(false) }

    val palette = listOf(
        MaterialTheme.colorScheme.primary,
        MaterialTheme.colorScheme.error,
        MaterialTheme.colorScheme.tertiary,
        MaterialTheme.colorScheme.secondary,
        MaterialTheme.colorScheme.inversePrimary,
    )

    if (cols.isEmpty()) {
        Box(modifier.fillMaxSize().padding(12.dp)) { Text("This file has no numeric columns to correlate.", fontSize = 13.sp) }
        return
    }
    Column(
        modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        if (csv.info.isNotEmpty()) Text(csv.label(), fontSize = 12.sp, fontWeight = FontWeight.Medium)

        Box {
            OutlinedButton(onClick = { expanded = true }) { Text("Fields (${chosen.size})") }
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                cols.forEach { c ->
                    DropdownMenuItem(
                        text = {
                            Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                                Checkbox(checked = c in chosen, onCheckedChange = null)
                                Spacer(Modifier.width(6.dp))
                                Text(c, fontFamily = FontFamily.Monospace, fontSize = 13.sp)
                            }
                        },
                        // Cap at the palette size: more lines than colours is not a chart, it is a knot.
                        onClick = {
                            chosen = if (c in chosen) chosen - c
                            else if (chosen.size < palette.size) chosen + c else chosen
                        },
                    )
                }
            }
        }

        val series = chosen.map { it to (csv.column(it) ?: emptyList()) }
        MultiLineChart(series.map { it.second }, palette, Modifier.fillMaxWidth().height(240.dp))

        // Pearson r of every series against the first-picked one — the pivot. |r|→1 is tight
        // correlation, sign says direction, ~0 says the two move independently.
        val pivot = series.firstOrNull()
        chosen.forEachIndexed { i, name ->
            val col = MetricFields.of(name)
            val vals = series[i].second.filterNotNull()
            val r = if (pivot != null && i > 0) pearson(pivot.second, series[i].second) else null
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Canvas(Modifier.size(12.dp).padding(top = 3.dp)) { drawRect(palette[i % palette.size]) }
                Column {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(name, fontFamily = FontFamily.Monospace, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                        if (i == 0) Text("pivot", fontSize = 11.sp, color = MaterialTheme.colorScheme.primary)
                        else if (r != null) Text("r=${fmt(r)} ${corrWord(r)}", fontSize = 11.sp, fontWeight = FontWeight.Medium)
                    }
                    if (col != null) Text(col.short, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (vals.isNotEmpty()) Text(
                        "min=${fmt(vals.min())}  max=${fmt(vals.max())}",
                        fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
        Text(
            "Each line is squashed to its own 0..1, so only the SHAPES compare — the legend keeps the " +
                "real range. r is vs the first field (the pivot): near ±1 they move together, near 0 they do not.",
            fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        // The run this file came from, in full — the correlations mean nothing without knowing the
        // configuration that produced them.
        Text("Configuration", fontSize = 13.sp, fontWeight = FontWeight.Bold)
        FullConfig(csv, MaterialTheme.colorScheme.primary, emptySet())
    }
}

/** How to say a correlation out loud, so the number is not the only cue. */
private fun corrWord(r: Float): String = when {
    abs(r) >= 0.8f -> if (r > 0) "moves together" else "moves opposite"
    abs(r) >= 0.4f -> if (r > 0) "loosely together" else "loosely opposite"
    else -> "unrelated"
}

/**
 * N series on one axis, each independently normalized to 0..1. Unlike the two-file compare (one
 * shared scale, because there the magnitudes ARE the comparison), here the magnitudes are unlike by
 * construction — cache_budget in GiB against dense_resident_frac in [0,1] — so each gets its own scale
 * and only the shapes are claimed to line up.
 */
@Composable
private fun MultiLineChart(series: List<List<Float?>>, palette: List<Color>, modifier: Modifier) {
    val grid = MaterialTheme.colorScheme.outlineVariant
    if (series.all { it.filterNotNull().isEmpty() }) {
        Box(modifier) { Text("pick at least one field", fontSize = 12.sp) }
        return
    }
    val n = maxOf(series.maxOfOrNull { it.size } ?: 2, 2)
    // Per-series min/max, computed once: normalization is min..max within each column.
    val ranges = series.map { s -> s.filterNotNull().let { v -> (v.minOrNull() ?: 0f) to (v.maxOrNull() ?: 1f) } }

    Canvas(modifier) {
        val w = size.width; val h = size.height
        listOf(0f, 0.5f, 1f).forEach { t -> drawLine(grid, Offset(0f, h * t), Offset(w, h * t), strokeWidth = 1f) }
        series.forEachIndexed { si, s ->
            // Each series on its own range: only the shapes are being claimed to line up.
            val (lo, hi) = ranges[si]
            drawSeries(s, n, lo, hi, palette[si % palette.size])
        }
    }
}
