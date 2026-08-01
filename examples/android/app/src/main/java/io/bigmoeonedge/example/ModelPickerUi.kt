package io.bigmoeonedge.example

// Model acquisition UI: the "Get a model" card and everything it opens. Split out of
// MainActivity, which owned the chat screen and this at the same time and read as one file with
// two unrelated jobs. Nothing here touches the chat or the engine: it downloads, imports and
// deletes model files, then tells the caller to re-scan.

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.interaction.DragInteraction
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.Locale

/**
 * "Get a model" card: one-tap downloads of the models this engine is measured on ([ModelCatalog]),
 * plus the escape hatches — any gguf URL (WorkManager) or a file the user already has (SAF
 * picker). Everything lands in the app models dir with no permission; [onModelReady] triggers a
 * re-scan so the new model shows up in the picker above.
 *
 * [models] is the current scan result: it is what tells a catalog entry it is already on device.
 */
@Composable
fun AddModelSection(
    models: List<File>,
    scanning: Boolean,
    loadedSig: String?,
    onModelReady: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    // A model whose delete is being confirmed: its filename, or null when no dialog is up.
    var deleteTarget by remember { mutableStateOf<String?>(null) }

    var url by rememberSaveable { mutableStateOf("") }
    var expanded by rememberSaveable { mutableStateOf(false) }
    var showInstall by rememberSaveable { mutableStateOf<String?>(null) }
    // Null until the first scan finishes: on a first run the card opens itself, on a device that
    // already has a model it stays out of the way. Deciding before the scan lands would flash the
    // whole catalog open on every launch.
    var open by rememberSaveable { mutableStateOf<Boolean?>(null) }
    LaunchedEffect(scanning) { if (!scanning && open == null) open = models.isEmpty() }
    val isOpen = open == true
    // The in-flight transfers, by filename. Driven by ModelDownloader's stream rather than
    // remembered here, so a download started before the app was killed shows up on its own and
    // finalization/completion is the downloader's business, not this card's.
    var progress by remember { mutableStateOf<Map<String, ModelDownloader.Progress>>(emptyMap()) }
    var importStatus by remember { mutableStateOf<String?>(null) }
    var importFrac by remember { mutableStateOf(-1f) }
    var error by remember { mutableStateOf<String?>(null) }
    // A catalog failure belongs to the row whose button was tapped: filename -> message. Reported
    // at the bottom of the card it would surface under a different heading entirely.
    var rowError by remember { mutableStateOf<Pair<String, String>?>(null) }

    // The raw on-disk view, shards included: `models` is the SELECTABLE list (non-first shards
    // hidden), but a sharded catalog entry is on-device only when every shard file is.
    //
    // Keyed on the in-flight NAMES as well as `models`, because `models` alone cannot see a shard
    // land: shards 2..N are hidden from the selectable list by design, so finishing a 41 GB shard
    // leaves it byte-identical and a `remember(models)` would serve a stale set forever — the entry
    // would offer "Download" for a model already fully on the device. The key set changes exactly
    // when a shard starts or finishes, not on every progress tick, so the directory scan stays rare.
    val present = remember(models, progress.keys) {
        models.map { it.name }.toSet() + ModelManager.allGgufNames(context)
    }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        error = null
        importStatus = "Importing…"
        importFrac = -1f
        scope.launch {
            SafImport.importGguf(context, uri) { copied, total ->
                importFrac = if (total > 0) (copied.toFloat() / total).coerceIn(0f, 1f) else -1f
            }.onSuccess {
                importStatus = null; importFrac = -1f; onModelReady()
            }.onFailure {
                importStatus = null; importFrac = -1f; error = it.message ?: "import failed"
            }
        }
    }

    // Follow the downloads. A landed one is already finalized (.part → .gguf) by the time it is
    // reported here, so this only has to re-scan; a failed one names itself in the error line.
    LaunchedEffect(Unit) {
        ModelDownloader.events(context).collect { ev ->
            when (ev) {
                is ModelDownloader.Event.InFlight -> progress = ev.downloads
                is ModelDownloader.Event.Completed -> onModelReady()
                is ModelDownloader.Event.Failed -> error = "${ev.name}: ${ev.reason}"
            }
        }
    }

    ElevatedCard(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            // Collapsible: once a model is on the device this card is just in the way, but it has
            // to stay reachable to add a second one.
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Get a model", fontWeight = FontWeight.SemiBold)
                    if (!isOpen && progress.isNotEmpty()) {
                        Text(
                            "${progress.size} downloading…",
                            fontSize = 12.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                TextButton(onClick = { open = !isOpen }) { Text(if (isOpen) "Close" else "Open") }
            }

            if (isOpen) {
                ModelCatalog.entries.forEach { e ->
                    CatalogRow(
                        entry = e,
                        status = ModelCatalog.statusOf(e, present, progress.keys),
                        progress = ModelDownloader.entryProgress(context, e, progress),
                        installShown = showInstall == e.fileName,
                        error = rowError?.takeIf { it.first == e.fileName }?.second,
                        onToggleInstall = { showInstall = if (showInstall == e.fileName) null else e.fileName },
                        onDownload = {
                            error = null
                            rowError = null
                            val res = if (e.shards.isNotEmpty()) {
                                ModelDownloader.enqueueShards(context, e)
                            } else {
                                ModelDownloader.enqueue(context, e.url ?: "", e.fileName, e.approxBytes)
                            }
                            res.onFailure {
                                rowError = e.fileName to (it.message ?: "download failed to start")
                            }
                        },
                        onCancel = { ModelDownloader.cancelEntry(context, e) },
                        onDelete = { deleteTarget = e.fileName },
                    )
                }

                // On-device models the catalog does not list — imported via the URL field or the
                // file picker below. They have no catalog row of their own, only a picker entry, so
                // this is the one place to remove them.
                val extraModels = models.filter { m -> !ModelCatalog.isCatalogFile(m.name) }
                if (extraModels.isNotEmpty()) {
                    HorizontalDivider()
                    Text("Imported models", fontSize = 14.sp, fontWeight = FontWeight.Medium)
                    extraModels.forEach { f ->
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) {
                                Text(f.name, fontSize = 14.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                Text(
                                    ModelCatalog.gbLabel(f.length()),
                                    fontSize = 12.sp,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                            TextButton(
                                onClick = { deleteTarget = f.name },
                                contentPadding = PaddingValues(horizontal = 12.dp),
                            ) { Text("Delete", maxLines = 1, softWrap = false) }
                        }
                    }
                }

                HorizontalDivider()

                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Other model", fontSize = 14.sp, modifier = Modifier.weight(1f))
                    TextButton(onClick = { expanded = !expanded }) { Text(if (expanded) "Hide" else "Show") }
                }
            }
            if (isOpen && expanded) {
                Text(
                    "Any MoE gguf works — paste a direct link, or pick a file already on the device.",
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                OutlinedTextField(
                    value = url,
                    onValueChange = { url = it },
                    label = { Text("gguf URL (e.g. a Hugging Face resolve link)") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Button(
                        onClick = {
                            error = null
                            ModelDownloader.enqueue(context, url)
                                .onFailure { error = it.message ?: "invalid URL" }
                        },
                        enabled = url.isNotBlank(),
                        modifier = Modifier.weight(1f),
                    ) { Text("Download") }
                    OutlinedButton(
                        onClick = { picker.launch(arrayOf("*/*")) },
                        modifier = Modifier.weight(1f),
                    ) { Text("Pick file") }
                }
                // A pasted URL has no catalog row to show its progress in. Shard names belong to
                // their entry's row: listing them here would duplicate the download AND offer a
                // Cancel that cancels nothing (the chain is registered under the entry name) while
                // still deleting the .part the worker is writing.
                progress.filterKeys { k -> !ModelCatalog.isCatalogFile(k) }
                    .forEach { (name, p) ->
                        DownloadProgress(p, onCancel = { ModelDownloader.cancel(context, name) })
                    }
            }

            importStatus?.let { st ->
                Text(st, fontSize = 12.sp)
                if (importFrac >= 0f) {
                    LinearProgressIndicator(progress = { importFrac }, modifier = Modifier.fillMaxWidth())
                } else {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                }
            }

            error?.let { Text(it, color = MaterialTheme.colorScheme.error, fontSize = 12.sp) }
        }
    }

    deleteTarget?.let { fname ->
        // A catalog entry deletes as a unit: every file it owns, not just the one whose button was
        // tapped. Orphaned 40 GB tails are the failure mode this forbids. The entry's own name stays
        // in the set alongside the shards, so a gpt-oss merged by an earlier release is still
        // deletable; copiesOf drops the names that are not on disk.
        val targetNames = remember(fname) {
            ModelCatalog.entries.firstOrNull { fname in ModelCatalog.fileNamesOf(it) }
                ?.let { ModelCatalog.fileNamesOf(it).toList() } ?: listOf(fname)
        }
        val copies = remember(fname, models) { targetNames.flatMap { ModelManager.copiesOf(context, it) } }
        // The loaded session pins its gguf via mmap; deleting it out from under a live engine is the
        // failure mode to forbid. sessionSignature starts with the model's path, so match on that.
        val isLoaded = copies.any { loadedSig != null && loadedSig.startsWith(it.absolutePath + "|") }
        DeleteModelDialog(
            fileName = fname,
            copies = copies,
            isLoaded = isLoaded,
            onDismiss = { deleteTarget = null },
            onConfirm = {
                val toDelete = copies.filter { ModelManager.isAppDeletable(it) }
                scope.launch {
                    withContext(Dispatchers.IO) { toDelete.forEach { runCatching { it.delete() } } }
                    deleteTarget = null
                    onModelReady() // rescan: the catalog status and the picker both refresh
                }
            },
        )
    }
}

/**
 * Confirm deleting every app-deletable copy of a model. Lists each copy with its size, flags copies
 * the app cannot remove (adb-pushed, shell-owned) and the loaded-model guard, and only enables the
 * delete when there is something to delete and the model is not in use.
 */
@Composable
private fun DeleteModelDialog(
    fileName: String,
    copies: List<File>,
    isLoaded: Boolean,
    onConfirm: () -> Unit,
    onDismiss: () -> Unit,
) {
    val deletable = copies.filter { ModelManager.isAppDeletable(it) }
    val blocked = copies.filterNot { ModelManager.isAppDeletable(it) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Delete $fileName?") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                if (isLoaded) {
                    Text(
                        "This model is loaded. Start a new chat (or switch models) before deleting it.",
                        color = MaterialTheme.colorScheme.error,
                        fontSize = 13.sp,
                    )
                }
                deletable.forEach { f ->
                    Text("${f.absolutePath}  ·  ${ModelCatalog.gbLabel(f.length())}", fontSize = 12.sp)
                }
                blocked.forEach { f ->
                    Text(
                        "${f.absolutePath} — adb-pushed; remove with: adb shell rm ${f.absolutePath}",
                        fontSize = 12.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                if (deletable.isEmpty() && !isLoaded) {
                    Text(
                        "Nothing here the app can delete.",
                        fontSize = 12.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onConfirm, enabled = !isLoaded && deletable.isNotEmpty()) {
                Text("Delete")
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** One catalog model: what it is, how big, and the single action its current state allows. */
@Composable
private fun CatalogRow(
    entry: ModelCatalog.Entry,
    status: ModelCatalog.Status,
    progress: ModelDownloader.Progress?,
    installShown: Boolean,
    error: String?,
    onToggleInstall: () -> Unit,
    onDownload: () -> Unit,
    onCancel: () -> Unit,
    onDelete: () -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        // Name and action share a line; the blurb gets the full card width on its own line. Next
        // to a button there is not enough room left for a sentence, and it wraps to a stack of
        // one-word lines.
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                entry.title,
                fontSize = 15.sp,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.width(8.dp))
            when (status) {
                ModelCatalog.Status.AVAILABLE ->
                    Button(onClick = onDownload, contentPadding = PaddingValues(horizontal = 16.dp)) {
                        Text("Download", maxLines = 1, softWrap = false)
                    }
                ModelCatalog.Status.DOWNLOADING ->
                    TextButton(onClick = onCancel) { Text("Cancel", maxLines = 1, softWrap = false) }
                ModelCatalog.Status.ON_DEVICE -> {
                    Text(
                        "On device",
                        fontSize = 13.sp,
                        maxLines = 1,
                        softWrap = false,
                        color = MaterialTheme.colorScheme.primary,
                    )
                    TextButton(onClick = onDelete, contentPadding = PaddingValues(horizontal = 12.dp)) {
                        Text("Delete", maxLines = 1, softWrap = false)
                    }
                }
                ModelCatalog.Status.MANUAL_ONLY ->
                    TextButton(onClick = onToggleInstall) {
                        Text(if (installShown) "Hide" else "How to", maxLines = 1, softWrap = false)
                    }
            }
        }
        Text(
            "${entry.quant} · ${ModelCatalog.sizeLabel(entry)} · ${entry.blurb}",
            fontSize = 12.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (error != null) {
            Text(error, fontSize = 12.sp, color = MaterialTheme.colorScheme.error)
        }
        if (progress != null) DownloadProgress(progress, onCancel = null, showName = false)
        if (installShown) {
            entry.install?.let {
                // Commands must stay copy-pasteable, so they scroll sideways rather than wrap:
                // a wrapped shell line is a broken shell line.
                SelectionContainer {
                    Text(
                        it,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.sp,
                        softWrap = false,
                        modifier = Modifier
                            .fillMaxWidth()
                            .horizontalScroll(rememberScrollState())
                            .padding(vertical = 4.dp),
                    )
                }
            }
        }
    }
}

/**
 * Progress line + bar for one download. [onCancel] null when the row already offers Cancel;
 * [showName] false inside a catalog row, where the title is already on the line above.
 */
@Composable
private fun DownloadProgress(
    p: ModelDownloader.Progress,
    onCancel: (() -> Unit)?,
    showName: Boolean = true,
) {
    val pct = if (p.totalBytes > 0) {
        (p.downloadedBytes.toFloat() / p.totalBytes).coerceIn(0f, 1f)
    } else {
        null
    }
    // Name and numbers go on separate lines: a long filename must never ellipsize away the MiB
    // counter, which is the part the user actually watches.
    if (showName) {
        Text(p.name, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
    Text(
        when {
            p.state == ModelDownloader.State.PAUSED -> p.reason ?: "paused"
            pct != null -> String.format(
                Locale.US, "%d%% (%d / %d MiB)",
                (pct * 100).toInt(), p.downloadedBytes shr 20, p.totalBytes shr 20,
            )
            p.downloadedBytes > 0 -> String.format(Locale.US, "%d MiB", p.downloadedBytes shr 20)
            else -> "Starting…"
        },
        fontSize = 12.sp,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
    )
    if (pct != null) {
        LinearProgressIndicator(progress = { pct }, modifier = Modifier.fillMaxWidth())
    } else {
        LinearProgressIndicator(Modifier.fillMaxWidth())
    }
    onCancel?.let { TextButton(onClick = it) { Text("Cancel") } }
}

