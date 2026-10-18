package io.bigmoeonedge.app.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.data.CatalogEntry
import io.bigmoeonedge.app.data.CatalogStatus
import io.bigmoeonedge.app.data.ModelDownloader
import io.bigmoeonedge.app.data.ModelManager
import io.bigmoeonedge.app.data.SafImport
import io.bigmoeonedge.app.data.catalogStatus
import io.bigmoeonedge.app.data.gbLabel
import io.bigmoeonedge.app.engine.Engine
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.Locale

/**
 * Models: what is on the phone (the server lists its own directory and the other places the app
 * started it with), the catalog to get one from, and the escape hatches for any other gguf. Loading
 * is one tap; with auto-plan on, the engine measures the phone and chooses the run before it loads.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ModelsScreen(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val engine by Engine.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }

    var progress by remember { mutableStateOf<Map<String, ModelDownloader.Progress>>(emptyMap()) }
    var refreshKey by remember { mutableStateOf(0) }
    var deleteTarget by remember { mutableStateOf<List<File>?>(null) }
    var importStatus by remember { mutableStateOf<String?>(null) }
    var importFrac by remember { mutableStateOf(-1f) }
    var url by rememberSaveable { mutableStateOf("") }
    var otherOpen by rememberSaveable { mutableStateOf(false) }

    fun say(msg: String) = scope.launch { snackbar.showSnackbar(msg) }

    // The server lists every place a model can sit; a refresh asks it again.
    LaunchedEffect(refreshKey, engine.serverUp) {
        Engine.refreshModels()
    }
    // Follow the app's own downloads: a landed one is finalized before it is reported.
    LaunchedEffect(Unit) {
        ModelDownloader.events(context).collect { ev ->
            when (ev) {
                is ModelDownloader.Event.InFlight -> progress = ev.downloads
                is ModelDownloader.Event.Completed -> refreshKey++
                is ModelDownloader.Event.Failed -> say("${ev.name}: ${ev.reason}")
            }
        }
    }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        importStatus = "Importing…"
        importFrac = -1f
        scope.launch {
            SafImport.importGguf(context, uri) { copied, total ->
                importFrac = if (total > 0) (copied.toFloat() / total).coerceIn(0f, 1f) else -1f
            }.onSuccess { importStatus = null; refreshKey++ }
                .onFailure { importStatus = null; say(it.message ?: "import failed") }
        }
    }

    val models = engine.models
    val loadedPath = engine.info?.model?.path
    val present = remember(models) { models?.fileNames.orEmpty() }

    Scaffold(
        modifier = modifier,
        topBar = {
            TopAppBar(
                title = { Text("Models") },
                navigationIcon = { Box(Modifier.padding(start = 16.dp)) { BrandMark(wordmark = false) } },
                actions = { TextButton(shape = RectangleShape, onClick = { requestSharedStorageAccess(context); refreshKey++ }) { Text("Refresh") } },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        Column(
            Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            if (!engine.serverUp) {
                Text("The engine is not running.", color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
            }
            if (engine.loading) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                    Text(
                        if (engine.info?.state == "planning") "Measuring the phone (storage, memory, each compute device), then loading…"
                        else "Loading the model…",
                        fontSize = 13.sp,
                    )
                }
            }

            // ── on the phone ──
            Text("On this phone", fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary)
            val local = models?.local.orEmpty()
            if (local.isEmpty()) {
                Text(
                    if (models == null) "Waiting for the engine…" else ModelManager.pushHint(),
                    fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            local.forEach { m ->
                LocalModelRow(
                    name = m.name,
                    detail = listOfNotNull(
                        gbLabel(m.bytes),
                        m.arch.ifEmpty { null },
                        if (m.shards > 1) "${m.shards} shards" else null,
                        if (!m.complete) "incomplete" else null,
                        if (m.complete && !m.streamable) "not a MoE this build streams" else null,
                        m.dir.takeIf { it.isNotEmpty() && it != models?.modelsDir },
                    ).joinToString(" · "),
                    loaded = m.path == loadedPath,
                    loadable = m.complete && engine.serverUp && !engine.loading,
                    onLoad = { scope.launch { Engine.load(m.path).onFailure { say(it.message ?: "load failed") } } },
                    onUnload = { scope.launch { Engine.unload() } },
                    // Every file the server listed for it: a split set deletes as a unit, never
                    // leaving a 40 GB tail, and the app never has to know how shards are named.
                    onDelete = m.files.map(::File).takeIf { ModelManager.canDelete(it) }?.let { files -> { deleteTarget = files } },
                )
            }

            // ── the catalog ──
            HorizontalDivider()
            Text("Get a model", fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary)
            Text(
                "The models this engine is measured on. Any other MoE gguf works too, below.",
                fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            models?.catalog.orEmpty().forEach { e ->
                CatalogRow(
                    entry = e,
                    status = catalogStatus(e, present, progress.keys),
                    progress = ModelDownloader.entryProgress(context, e, progress),
                    onDownload = {
                        ModelDownloader.enqueueEntry(context, e).onFailure { say("${e.title}: ${it.message ?: "download failed to start"}") }
                    },
                    onCancel = { ModelDownloader.cancelEntry(context, e) },
                    onDelete = {
                        // The app's own downloads, in its models directory; a copy elsewhere is listed above.
                        val dir = ModelManager.internalModelsDir(context)
                        deleteTarget = e.files.map { File(dir, it.name) }.filter { it.isFile }
                    },
                )
            }

            // ── anything else ──
            HorizontalDivider()
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Other model", fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary, modifier = Modifier.weight(1f))
                TextButton(shape = RectangleShape, onClick = { otherOpen = !otherOpen }) { Text(if (otherOpen) "Hide" else "Show") }
            }
            if (otherOpen) {
                Text("Paste a direct link to a gguf, or pick a file already on the phone.", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                OutlinedTextField(
                    value = url, onValueChange = { url = it },
                    label = { Text("gguf URL (e.g. a Hugging Face resolve link)") },
                    singleLine = true, modifier = Modifier.fillMaxWidth(),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Button(shape = RectangleShape, 
                        onClick = { ModelDownloader.enqueue(context, url).onFailure { say(it.message ?: "invalid URL") } },
                        enabled = url.isNotBlank(), modifier = Modifier.weight(1f),
                    ) { Text("Download") }
                    OutlinedButton(shape = RectangleShape, onClick = { picker.launch(arrayOf("*/*")) }, modifier = Modifier.weight(1f)) { Text("Pick file") }
                }
                val catalogNames = models?.catalog.orEmpty().flatMap { it.fileNames() }.toSet()
                progress.filterKeys { it !in catalogNames }.forEach { (name, p) ->
                    DownloadProgress(p, onCancel = { ModelDownloader.cancel(context, name) })
                }
            }
            importStatus?.let { st ->
                Text(st, fontSize = 12.sp)
                if (importFrac >= 0f) LinearProgressIndicator(progress = { importFrac }, modifier = Modifier.fillMaxWidth())
                else LinearProgressIndicator(Modifier.fillMaxWidth())
            }
            Spacer(Modifier.height(24.dp))
        }
    }

    deleteTarget?.let { files ->
        val isLoaded = files.any { it.absolutePath == loadedPath }
        AlertDialog(
            onDismissRequest = { deleteTarget = null },
            title = { Text("Delete ${files.firstOrNull()?.name ?: "model"}?") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    if (isLoaded) Text("This model is loaded. Unload it first.", color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
                    files.forEach { f -> Text("${f.name}  ·  ${gbLabel(f.length())}", fontSize = 12.sp) }
                    if (files.isEmpty()) Text("Nothing here the app can delete.", fontSize = 12.sp)
                }
            },
            confirmButton = {
                TextButton(shape = RectangleShape, 
                    onClick = {
                        scope.launch {
                            val kept = withContext(Dispatchers.IO) { files.filter { it.exists() && !it.delete() } }
                            if (kept.isNotEmpty()) say("Could not delete ${kept.joinToString { it.name }}")
                            deleteTarget = null
                            refreshKey++
                        }
                    },
                    enabled = !isLoaded && files.isNotEmpty(),
                ) { Text("Delete") }
            },
            dismissButton = { TextButton(shape = RectangleShape, onClick = { deleteTarget = null }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun LocalModelRow(
    name: String,
    detail: String,
    loaded: Boolean,
    loadable: Boolean,
    onLoad: () -> Unit,
    onUnload: () -> Unit,
    onDelete: (() -> Unit)?,
) {
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(name, fontSize = 14.sp, fontWeight = FontWeight.Medium, maxLines = 2, overflow = TextOverflow.Ellipsis)
                Text(detail, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Spacer(Modifier.width(8.dp))
            if (loaded) {
                Text("Loaded", fontSize = 13.sp, color = MaterialTheme.colorScheme.primary)
                TextButton(shape = RectangleShape, onClick = onUnload, contentPadding = PaddingValues(horizontal = 12.dp)) { Text("Unload") }
            } else {
                Button(shape = RectangleShape, onClick = onLoad, enabled = loadable, contentPadding = PaddingValues(horizontal = 16.dp)) { Text("Load") }
                if (onDelete != null) {
                    TextButton(shape = RectangleShape, onClick = onDelete, contentPadding = PaddingValues(horizontal = 8.dp)) { Text("Delete") }
                }
            }
        }
    }
}

/** One catalog model: what it is, how big, and the single action its state allows. */
@Composable
private fun CatalogRow(
    entry: CatalogEntry,
    status: CatalogStatus,
    progress: ModelDownloader.Progress?,
    onDownload: () -> Unit,
    onCancel: () -> Unit,
    onDelete: () -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(entry.title, fontSize = 15.sp, fontWeight = FontWeight.Medium, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
            Spacer(Modifier.width(8.dp))
            when (status) {
                CatalogStatus.AVAILABLE -> Button(shape = RectangleShape, onClick = onDownload, contentPadding = PaddingValues(horizontal = 16.dp)) { Text("Download", maxLines = 1, softWrap = false) }
                CatalogStatus.DOWNLOADING -> TextButton(shape = RectangleShape, onClick = onCancel) { Text("Cancel", maxLines = 1, softWrap = false) }
                CatalogStatus.ON_DEVICE -> {
                    Text("On device", fontSize = 13.sp, maxLines = 1, softWrap = false, color = MaterialTheme.colorScheme.primary)
                    TextButton(shape = RectangleShape, onClick = onDelete, contentPadding = PaddingValues(horizontal = 12.dp)) { Text("Delete", maxLines = 1, softWrap = false) }
                }
            }
        }
        Text("${entry.quant} · ~${gbLabel(entry.bytes)} · ${entry.blurb}", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (progress != null) DownloadProgress(progress, onCancel = null, showName = false)
    }
}

@Composable
private fun DownloadProgress(p: ModelDownloader.Progress, onCancel: (() -> Unit)?, showName: Boolean = true) {
    val pct = if (p.totalBytes > 0) (p.downloadedBytes.toFloat() / p.totalBytes).coerceIn(0f, 1f) else null
    if (showName) Text(p.name, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
    Text(
        when {
            p.state == ModelDownloader.State.PAUSED -> p.reason ?: "paused"
            pct != null -> String.format(Locale.US, "%d%% (%d / %d MiB)", (pct * 100).toInt(), p.downloadedBytes shr 20, p.totalBytes shr 20)
            p.downloadedBytes > 0 -> String.format(Locale.US, "%d MiB", p.downloadedBytes shr 20)
            else -> "Starting…"
        },
        fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
    )
    if (pct != null) LinearProgressIndicator(progress = { pct }, modifier = Modifier.fillMaxWidth())
    else LinearProgressIndicator(Modifier.fillMaxWidth())
    onCancel?.let { TextButton(shape = RectangleShape, onClick = it) { Text("Cancel") } }
}

/**
 * Request all-files access, needed only by the dev flavor to scan device storage for adb-pushed
 * models. Called on an explicit user action (Refresh), never at startup. No-op on the Play flavor
 * and when access is already granted.
 */
fun requestSharedStorageAccess(context: android.content.Context) {
    if (!io.bigmoeonedge.app.BuildConfig.SHARED_STORAGE) return
    if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R && !android.os.Environment.isExternalStorageManager()) {
        runCatching {
            context.startActivity(
                android.content.Intent(
                    android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    android.net.Uri.parse("package:${context.packageName}"),
                ).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
            )
        }
    }
}
