package io.bigmoeonedge.app.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.data.CatalogEntry
import io.bigmoeonedge.app.data.FileRole
import io.bigmoeonedge.app.data.LibraryRow
import io.bigmoeonedge.app.data.ModelDownloader
import io.bigmoeonedge.app.data.ModelManager
import io.bigmoeonedge.app.data.RowState
import io.bigmoeonedge.app.data.SafImport
import io.bigmoeonedge.app.data.gbLabel
import io.bigmoeonedge.app.data.libraryRows
import io.bigmoeonedge.app.data.resumeLabel
import io.bigmoeonedge.app.engine.ApiException
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.engine.LocalModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.Locale

/** What a delete removes, asked before it happens: by the server for its own directory, by the app elsewhere. */
private data class DeleteTarget(val title: String, val files: List<File>, val serverName: String?)

/**
 * Models: one list of what is on the phone and what the catalog offers, merged by file
 * (docs/screens.md, "Models"; data/Library.kt), each row with its one state and primary action;
 * then the ways to add to it: a Hugging Face repository, a direct link, a file on the phone. The
 * server lists every place a model can sit; the app does the transfers. Loading is one tap; with
 * auto-plan on, the engine measures the phone and chooses the run before it loads.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ModelsScreen(modifier: Modifier = Modifier, onOpenChat: () -> Unit = {}) {
    val context = LocalContext.current
    val engine by Engine.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }

    var progress by remember { mutableStateOf<Map<String, ModelDownloader.Progress>>(emptyMap()) }
    var refreshKey by remember { mutableStateOf(0) }
    var deleteTarget by remember { mutableStateOf<DeleteTarget?>(null) }
    var importStatus by remember { mutableStateOf<String?>(null) }
    var importFrac by remember { mutableStateOf(-1f) }
    var otherOpen by rememberSaveable { mutableStateOf(false) }
    var whereOpen by rememberSaveable { mutableStateOf(false) }
    // The entry the person just started: its row is scrolled to, now and again when it lands.
    var focusId by rememberSaveable { mutableStateOf("") }

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
                // The server lists the .part it left, so the row offers Resume with its bytes.
                is ModelDownloader.Event.Failed -> { say("${ev.name}: ${ev.reason}"); refreshKey++ }
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

    /** Start an entry's download, the app's own transfer; a refusal (no space) is thrown to the caller. */
    suspend fun startEntry(e: CatalogEntry) {
        withContext(Dispatchers.IO) { ModelDownloader.enqueueEntry(context, e) }.getOrThrow()
        focusId = e.id
    }
    fun download(e: CatalogEntry) = scope.launch {
        runCatching { startEntry(e) }.onFailure { say("${e.title}: ${it.message ?: "download failed to start"}") }
    }
    // A model added from a repository or a link is a catalog entry like the others, downloaded in the
    // same breath; a download that does not start throws back to the card that asked.
    val added: suspend (String) -> Unit = { id ->
        val e = Engine.state.value.models?.catalog?.firstOrNull { it.id == id } ?: error("the server does not list entry $id")
        startEntry(e)
    }

    val models = engine.models
    val loadedPath = engine.info?.model?.path
    // Projectors and draft heads (by their headers) are no model to load: listed apart, after the models.
    val (localModels, companions) = remember(models) { FileRole.split(models?.local.orEmpty()) { it.role } }
    val rows = remember(models, progress.keys, loadedPath) { models?.let { libraryRows(it, localModels, progress.keys, loadedPath) }.orEmpty() }
    val yours = rows.filter { !it.suggested }
    val suggested = rows.filter { it.suggested }
    // A pasted link the app downloads by itself has no catalog entry: its transfer is shown on its own.
    val catalogNames = models?.catalog.orEmpty().flatMap { it.fileNames() }.toSet()
    val strays = progress.filterKeys { it !in catalogNames }
    // Nothing here and nothing coming: the screen is the three steps to a first model.
    val empty = models != null && yours.isEmpty() && companions.isEmpty() && strays.isEmpty()
    val canLoad = engine.serverUp && !engine.loading

    fun askDelete(title: String, m: LocalModel, serverName: String?) {
        val files = m.files.map(::File)
        // The server deletes in its own directory; elsewhere the app deletes what it may write.
        if (serverName != null || ModelManager.canDelete(files)) deleteTarget = DeleteTarget(title, files, serverName)
    }

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
            if (engine.info?.state == "error") {
                Text("Last load failed: ${engine.info?.error.orEmpty()}", color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
            }
            if (engine.loading) {
                // Seconds since the load began, so a long load reads as progress rather than as a hang.
                var elapsed by remember { mutableStateOf(0) }
                LaunchedEffect(Unit) {
                    val t0 = System.currentTimeMillis()
                    while (true) {
                        elapsed = ((System.currentTimeMillis() - t0) / 1000).toInt()
                        delay(1000)
                    }
                }
                val planning = engine.info?.state == "planning"
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                    Text((if (planning) "Measuring this phone, then loading" else "Loading the model") + " · $elapsed s", fontSize = 13.sp)
                }
                if (planning) PlanProgressCard(engine.planProgress)
            }
            engine.info?.model?.let { m ->
                // A model opened from outside the listed places has no row to carry these.
                if (rows.none { it.state == RowState.LOADED }) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Loaded: ${m.name}", fontSize = 13.sp, modifier = Modifier.weight(1f))
                        TextButton(shape = RectangleShape, onClick = { scope.launch { Engine.unload() } }) { Text("Unload") }
                        TextButton(shape = RectangleShape, onClick = onOpenChat) { Text("Go to Chat") }
                    }
                }
            }

            val rowView: @Composable (LibraryRow) -> Unit = { r ->
                ModelRow(
                    r,
                    progress = r.entry?.let { ModelDownloader.entryProgress(context, it, progress) },
                    modelsDir = models?.modelsDir.orEmpty(),
                    followed = r.entry != null && r.entry.id == focusId,
                    canLoad = canLoad,
                    onLoad = { path -> scope.launch { Engine.load(path).onFailure { say(it.message ?: "load failed") } } },
                    onUnload = { scope.launch { Engine.unload() } },
                    onOpenChat = onOpenChat,
                    onDownload = { download(it) },
                    onCancel = { e ->
                        scope.launch {
                            withContext(Dispatchers.IO) { ModelDownloader.cancelEntry(context, e) }
                            refreshKey++ // the kept .part turns the row into Resume
                        }
                    },
                    onDeletePartial = { e -> scope.launch { Engine.deletePartial(e.id).onFailure { say(it.message ?: "could not delete the partial download") } } },
                    onDelete = r.local?.takeIf { m -> r.deleteName != null || ModelManager.canDelete(m.files.map(::File)) }?.let { m -> { askDelete(r.title, m, r.deleteName) } },
                    onForget = { e -> scope.launch { Engine.removeAdded(e.id).onFailure { say(it.message ?: "could not forget it") } } },
                )
            }

            when {
                models == null -> Text("Waiting for the engine…", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                empty -> {
                    Section("Get your first model")
                    Steps(
                        listOf(
                            "Find a model: search by name, or paste a repository.",
                            "Check it fits here: the server reads its header and measures it against this phone, before any download.",
                            "Download, then Load: Load appears on its row when it is done.",
                        ),
                    )
                    HuggingFaceSection(onAdded = added)
                    if (suggested.isNotEmpty()) {
                        Text("Or start from a suggestion", fontWeight = FontWeight.Medium, fontSize = 14.sp)
                        suggested.forEach { r ->
                            val e = r.entry ?: return@forEach
                            OutlinedButton(shape = RectangleShape, onClick = { download(e) }, modifier = Modifier.fillMaxWidth()) {
                                Text("Download ${e.title}" + (if (e.quant.isNotEmpty()) " (${e.quant})" else "") + ", ${gbLabel(e.bytes)}", maxLines = 2)
                            }
                        }
                    }
                    Text("Or a direct link to a gguf", fontWeight = FontWeight.Medium, fontSize = 14.sp)
                    LinkImport(onAdded = { e -> startEntry(e) })
                    OutlinedButton(shape = RectangleShape, onClick = { picker.launch(arrayOf("*/*")) }, modifier = Modifier.fillMaxWidth()) { Text("Or pick a gguf on the phone") }
                }
                else -> {
                    Section("Your models")
                    yours.forEach { rowView(it) }
                    strays.forEach { (name, p) -> DownloadProgress(p, onCancel = { ModelDownloader.cancel(context, name) }) }
                    if (companions.isNotEmpty()) {
                        Hint("${companions.size} companion ${if (companions.size == 1) "file" else "files"}: not models to load, by their headers.")
                        companions.forEach { m ->
                            CompanionRow(m, onDelete = m.takeIf { serverDeleteName(it, models.modelsDir) != null || ModelManager.canDelete(it.files.map(::File)) }
                                ?.let { { askDelete(m.name, m, serverDeleteName(m, models.modelsDir)) } })
                        }
                    }
                    if (suggested.isNotEmpty()) {
                        Section("Suggested")
                        suggested.forEach { rowView(it) }
                    }
                    HorizontalDivider()
                    Section("Get another model")
                    HuggingFaceSection(onAdded = added)
                    HorizontalDivider()
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Any other model", fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary, modifier = Modifier.weight(1f))
                        TextButton(shape = RectangleShape, onClick = { otherOpen = !otherOpen }) { Text(if (otherOpen) "Hide" else "Show") }
                    }
                    if (otherOpen) {
                        Hint("A direct link to a gguf, or a file already on the phone.")
                        LinkImport(onAdded = { e -> startEntry(e) })
                        OutlinedButton(shape = RectangleShape, onClick = { picker.launch(arrayOf("*/*")) }, modifier = Modifier.fillMaxWidth()) { Text("Pick a file") }
                    }
                }
            }
            importStatus?.let { st ->
                Text(st, fontSize = 12.sp)
                if (importFrac >= 0f) LinearProgressIndicator(progress = { importFrac }, modifier = Modifier.fillMaxWidth())
                else LinearProgressIndicator(Modifier.fillMaxWidth())
            }

            models?.let { m ->
                Text(
                    (if (whereOpen) "v" else ">") + "  Where models live",
                    fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.fillMaxWidth().clickable { whereOpen = !whereOpen }.padding(vertical = 4.dp),
                )
                if (whereOpen) {
                    Hint("Downloads and imports land in this app's own storage, where streamed reads run fastest:")
                    Text(m.modelsDir, fontFamily = FontFamily.Monospace, fontSize = 11.sp)
                    if (m.scanDirs.isNotEmpty()) {
                        Hint("Also listed, and loaded in place:")
                        m.scanDirs.forEach { Text(it, fontFamily = FontFamily.Monospace, fontSize = 11.sp) }
                    }
                    Hint(ModelManager.pushHint())
                }
            }
            Spacer(Modifier.height(24.dp))
        }
    }

    deleteTarget?.let { t ->
        val isLoaded = t.files.any { it.absolutePath == loadedPath }
        AlertDialog(
            onDismissRequest = { deleteTarget = null },
            title = { Text("Delete ${t.title}?") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    if (isLoaded) Text("This model is loaded. Unload it first.", color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
                    t.files.forEach { f -> Text("${f.name}  ·  ${gbLabel(f.length())}", fontSize = 12.sp) }
                }
            },
            confirmButton = {
                TextButton(
                    shape = RectangleShape,
                    onClick = {
                        scope.launch {
                            val name = t.serverName
                            if (name != null) {
                                Engine.deleteLocal(name).onFailure { say(it.message ?: "could not delete it") }
                            } else {
                                val kept = withContext(Dispatchers.IO) { t.files.filter { it.exists() && !it.delete() } }
                                if (kept.isNotEmpty()) say("Could not delete ${kept.joinToString { it.name }}")
                            }
                            deleteTarget = null
                            refreshKey++
                        }
                    },
                    enabled = !isLoaded,
                ) { Text("Delete") }
            },
            dismissButton = { TextButton(shape = RectangleShape, onClick = { deleteTarget = null }) { Text("Keep") } },
        )
    }
}

/** The listing's name for a model the server's own directory holds (`folder/name`), or null elsewhere. */
private fun serverDeleteName(m: LocalModel, modelsDir: String): String? =
    if (m.dir == modelsDir) (if (m.folder.isNotEmpty()) "${m.folder}/${m.name}" else m.name) else null

@Composable
private fun Section(title: String) {
    Text(title, fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary)
}

/** Numbered steps, one line each. */
@Composable
fun Steps(lines: List<String>) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        lines.forEachIndexed { i, s ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("${i + 1}", fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary)
                Text(s, fontSize = 13.sp)
            }
        }
    }
}

@Composable
private fun stateColour(s: RowState) = when (s) {
    RowState.AVAILABLE -> MaterialTheme.colorScheme.onSurfaceVariant
    RowState.DOWNLOADING -> MaterialTheme.colorScheme.primary
    RowState.PARTIAL -> warnColor()
    RowState.ON_DEVICE, RowState.LOADED -> okColor()
    RowState.INCOMPLETE -> MaterialTheme.colorScheme.error
}

/**
 * One row of the list: what it is, its state, what that state shows (progress, what is here of the
 * whole), and the one primary action for it, with Delete or Forget beside it where they apply.
 */
@OptIn(ExperimentalFoundationApi::class, ExperimentalLayoutApi::class)
@Composable
private fun ModelRow(
    r: LibraryRow,
    progress: ModelDownloader.Progress?,
    modelsDir: String,
    followed: Boolean,
    canLoad: Boolean,
    onLoad: (String) -> Unit,
    onUnload: () -> Unit,
    onOpenChat: () -> Unit,
    onDownload: (CatalogEntry) -> Unit,
    onCancel: (CatalogEntry) -> Unit,
    onDeletePartial: (CatalogEntry) -> Unit,
    onDelete: (() -> Unit)?,
    onForget: (CatalogEntry) -> Unit,
) {
    val e = r.entry
    val m = r.local
    val bring = remember { BringIntoViewRequester() }
    // The followed row is scrolled to when it is started and again when it changes state (lands, fails).
    LaunchedEffect(followed, r.state) { if (followed) runCatching { bring.bringIntoView() } }
    val edge = if (followed || r.state == RowState.LOADED) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline
    Column(
        Modifier.fillMaxWidth().bringIntoViewRequester(bring).border(2.dp, edge).padding(10.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(r.title, fontSize = 14.sp, fontWeight = FontWeight.Medium, maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
            Chip(r.state.label, stateColour(r.state))
        }
        val size = m?.bytes ?: e?.bytes ?: 0
        Text(
            listOfNotNull(
                e?.quant?.ifEmpty { null },
                gbLabel(size),
                m?.arch?.ifEmpty { null },
                m?.takeIf { it.complete }?.let { if (it.streamable) "streams" else "not a MoE this build streams" },
                m?.takeIf { r.state == RowState.INCOMPLETE || r.state == RowState.PARTIAL }?.let { "${it.files.size} of ${it.shards} shards here" }
                    ?: m?.shards?.takeIf { it > 1 }?.let { "$it shards" },
                m?.name?.takeIf { it != r.title },
                m?.folder?.ifEmpty { null }?.let { "in $it" },
                m?.dir?.takeIf { it.isNotEmpty() && it != modelsDir }?.let { "in $it" },
                e?.added?.takeIf { it }?.let { "added" },
            ).joinToString(" · "),
            fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (m == null && e != null && e.blurb.isNotEmpty()) Text(e.blurb, fontSize = 12.sp)
        when {
            r.state == RowState.DOWNLOADING && progress != null -> DownloadProgress(progress, onCancel = null, showName = false)
            r.state == RowState.PARTIAL && e != null && e.bytes > 0 -> {
                LinearProgressIndicator(progress = { (e.received.toFloat() / e.bytes).coerceIn(0f, 1f) }, modifier = Modifier.fillMaxWidth())
            }
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            when {
                r.state == RowState.LOADED && m != null -> {
                    OutlinedButton(shape = RectangleShape, onClick = { onLoad(m.path) }, enabled = canLoad) { Text("Reload") }
                    OutlinedButton(shape = RectangleShape, onClick = onUnload) { Text("Unload") }
                    Button(shape = RectangleShape, onClick = onOpenChat) { Text("Go to Chat") }
                }
                r.state == RowState.ON_DEVICE && m != null -> Button(shape = RectangleShape, onClick = { onLoad(m.path) }, enabled = canLoad) { Text("Load") }
                r.state == RowState.DOWNLOADING && e != null -> OutlinedButton(shape = RectangleShape, onClick = { onCancel(e) }) { Text("Cancel") }
                e != null && r.state in setOf(RowState.AVAILABLE, RowState.PARTIAL, RowState.INCOMPLETE) -> {
                    Button(shape = RectangleShape, onClick = { onDownload(e) }) {
                        Text(if (e.partial) resumeLabel(e.received, e.bytes) else "Download", maxLines = 1, softWrap = false)
                    }
                    if (e.partial) TextButton(shape = RectangleShape, onClick = { onDeletePartial(e) }) { Text("Delete partial") }
                }
            }
            if (onDelete != null && r.state != RowState.LOADED && r.state != RowState.DOWNLOADING) {
                TextButton(shape = RectangleShape, onClick = onDelete) { Text("Delete", color = MaterialTheme.colorScheme.error) }
            }
            if (e != null && e.added && r.state != RowState.DOWNLOADING) {
                TextButton(shape = RectangleShape, onClick = { onForget(e) }) { Text("Forget") }
            }
        }
    }
}

/** A file that goes with a model and is not one (a projector, a draft head): what it is, and Delete. */
@Composable
private fun CompanionRow(m: LocalModel, onDelete: (() -> Unit)?) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(m.name, fontSize = 13.sp, maxLines = 2, overflow = TextOverflow.Ellipsis)
            Hint(listOfNotNull(FileRole.label(m.role), gbLabel(m.bytes), m.arch.ifEmpty { null }, m.folder.ifEmpty { null }?.let { "in $it" }).joinToString(" · "))
        }
        if (onDelete != null) TextButton(shape = RectangleShape, onClick = onDelete) { Text("Delete", color = MaterialTheme.colorScheme.error) }
    }
}

/**
 * Any other model: a direct https link to a gguf, or to any shard of a split set. The server makes it
 * a catalog entry (`POST /api/models/url`), and [onAdded] downloads its files the way every entry is
 * downloaded here, with resume and one progress bar over the set. When the server cannot reach the
 * host itself, the app fetches the one file the link names on its own.
 */
@Composable
private fun LinkImport(onAdded: suspend (CatalogEntry) -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var url by rememberSaveable { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }
    fun add() = scope.launch {
        busy = true
        error = ""
        val link = url.trim()
        Engine.addUrl(link).fold(
            onSuccess = { e -> runCatching { onAdded(e) }.onSuccess { url = "" }.onFailure { error = it.message ?: "download failed to start" } },
            onFailure = { t ->
                if (t is ApiException && t.status >= 500) {
                    // The server's own fetch could not ask the host (no fetcher in its process, a host it
                    // cannot reach): the app's downloader, which has the phone's network, takes the file.
                    withContext(Dispatchers.IO) { ModelDownloader.enqueue(context, link) }.onSuccess { url = "" }.onFailure { error = it.message ?: "invalid URL" }
                } else {
                    error = t.message ?: "the link was refused"
                }
            },
        )
        busy = false
    }
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        OutlinedTextField(
            value = url, onValueChange = { url = it },
            label = { Text("https://…/model.gguf") },
            singleLine = true, modifier = Modifier.fillMaxWidth(),
        )
        Button(shape = RectangleShape, onClick = { add() }, enabled = url.isNotBlank() && !busy, modifier = Modifier.fillMaxWidth()) {
            Text(if (busy) "Checking the link…" else "Download")
        }
        Hint("A link to one file, or to any shard of a split set (the other shards are fetched beside it). It is added to Your models and downloads like any other.")
        if (error.isNotEmpty()) Text(error, fontSize = 12.sp, color = MaterialTheme.colorScheme.error)
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
