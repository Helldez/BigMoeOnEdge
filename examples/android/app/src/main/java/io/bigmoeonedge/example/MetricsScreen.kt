package io.bigmoeonedge.example

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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Reads back the per-token CSV the engine wrote for a session (`--csv`, see [AppSettings]).
 *
 * Deliberately a viewer, not a dashboard: it shows the file as it is, because the point of the file
 * is to be the evidence. A summary that averaged these rows would hide exactly the thing worth
 * seeing — a run's shape is in the individual token where the budget moved and the faults spiked,
 * not in its mean.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MetricsScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    var picked by remember { mutableStateOf<File?>(null) }
    val files = remember {
        File(context.getExternalFilesDir(null), "metrics")
            .listFiles { f -> f.name.endsWith(".csv") }
            ?.sortedByDescending { it.lastModified() }
            ?: emptyList()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(picked?.name ?: "Metrics") },
                navigationIcon = {
                    IconButton(onClick = { if (picked != null) picked = null else onBack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        val file = picked
        if (file == null) {
            FileList(files, Modifier.padding(padding)) { picked = it }
        } else {
            CsvView(file, Modifier.padding(padding))
        }
    }
}

@Composable
private fun FileList(files: List<File>, modifier: Modifier, onPick: (File) -> Unit) {
    if (files.isEmpty()) {
        Column(modifier.fillMaxSize().padding(24.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("No metrics yet.", fontWeight = FontWeight.Medium)
            Text(
                "Turn on Settings → Diagnostics → Metrics CSV, then run a prompt. One file is written " +
                    "per session, covering every turn in it.",
                fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        return
    }
    val stamp = SimpleDateFormat("d MMM HH:mm:ss", Locale.getDefault())
    LazyColumn(modifier.fillMaxSize()) {
        items(files) { f ->
            ListItem(
                headlineContent = { Text(f.name, fontFamily = FontFamily.Monospace, fontSize = 13.sp) },
                supportingContent = {
                    Text(
                        "${stamp.format(Date(f.lastModified()))} · ${f.length() / 1024} KiB",
                        fontSize = 12.sp,
                    )
                },
                modifier = Modifier.clickable(onClick = { onPick(f) }),
            )
            HorizontalDivider()
        }
    }
}

@Composable
private fun CsvView(file: File, modifier: Modifier) {
    // Read once per open. These files are one row per token — thousands at worst, tens of KiB — so
    // the whole thing fits in memory and there is nothing to gain from streaming it.
    val lines = remember(file) { runCatching { file.readLines() }.getOrElse { listOf("cannot read: ${it.message}") } }
    val header = lines.firstOrNull()?.split(",") ?: emptyList()
    val rows = lines.drop(1).filter { it.isNotBlank() && !it.startsWith("#") }.map { it.split(",") }
    // The engine writes one `# summary key=value ...` trailer per turn: the run-level facts
    // (tok/s, token_demand_MiB, cache_cuts) that no single row carries.
    val summaries = lines.filter { it.startsWith("# summary") }

    val hscroll = rememberScrollState()
    Column(modifier.fillMaxSize()) {
        summaries.forEach { s ->
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
            "${rows.size} tokens · ${header.size} columns · scroll sideways for all of them",
            fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
        )
        // One horizontal scroll shared by the header and every row, so the columns stay aligned.
        Column(Modifier.horizontalScroll(hscroll)) {
            Row(Modifier.padding(horizontal = 8.dp)) {
                header.forEach { h -> Cell(h, bold = true) }
            }
            HorizontalDivider()
            LazyColumn(Modifier.fillMaxWidth()) {
                items(rows) { r ->
                    Row(Modifier.padding(horizontal = 8.dp)) {
                        r.forEach { v -> Cell(v) }
                    }
                }
            }
        }
    }
}

// Fixed-width cells: a table of numbers that does not line up is a table you have to read twice.
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
