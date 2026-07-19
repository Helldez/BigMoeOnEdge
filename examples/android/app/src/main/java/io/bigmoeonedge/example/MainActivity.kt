package io.bigmoeonedge.example

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
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.Locale

/**
 * Minimal chat + live telemetry, in Compose. Pick a pushed .gguf, type a prompt, run:
 * the panel shows tok/s and the per-token compute-vs-flash-I/O split and cache hit rate
 * while the answer streams in. All tunables live on the Settings screen.
 */
class MainActivity : ComponentActivity() {

    private val requestNotif =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestNotif.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
        // All-files access is NOT requested at startup: downloaded, imported and picked models
        // live in the app-specific dir and need no permission. The dev flavor asks for it only
        // when the user explicitly rescans device storage (Refresh) for adb-pushed models.
        setContent {
            MaterialTheme(colorScheme = if (isSystemDark()) darkColorScheme() else lightColorScheme()) {
                Surface(color = MaterialTheme.colorScheme.background) { Root() }
            }
        }
    }

    private fun isSystemDark(): Boolean {
        val flag = resources.configuration.uiMode and
            android.content.res.Configuration.UI_MODE_NIGHT_MASK
        return flag == android.content.res.Configuration.UI_MODE_NIGHT_YES
    }
}

/**
 * Request all-files access, needed only by the dev flavor to scan device storage for adb-pushed
 * models. Called on an explicit user action (Refresh), never at startup. No-op on the Play flavor
 * and when access is already granted.
 */
fun requestSharedStorageAccess(context: android.content.Context) {
    if (!BuildConfig.SHARED_STORAGE) return
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
        runCatching {
            context.startActivity(
                Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:${context.packageName}"),
                ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            )
        }
    }
}

@Composable
private fun Root() {
    val context = LocalContext.current
    var showSettings by remember { mutableStateOf(false) }
    var showMetrics by remember { mutableStateOf(false) }
    var settings by remember { mutableStateOf(AppSettings.load(context)) }

    // Model-scan state lives here, above the settings/main switch, so opening Settings and
    // coming back does NOT dispose it and trigger a fresh scan. The scan runs once (and again
    // only when refreshKey changes: an explicit Refresh, or after a download/import completes).
    var models by remember { mutableStateOf<List<File>>(emptyList()) }
    var scanning by remember { mutableStateOf(true) }
    var refreshKey by remember { mutableStateOf(0) }
    var modelIdx by remember { mutableStateOf(0) }

    // Probing gguf headers to keep only MoE models does blocking reads — off the main thread.
    LaunchedEffect(refreshKey) {
        scanning = true
        models = withContext(Dispatchers.IO) { ModelManager.listMoeModels(context) }
        if (modelIdx >= models.size) modelIdx = 0
        scanning = false
    }

    // Only this one field of the session state, so Root does not recompose on every token.
    val thinkCtl by remember { RunBus.state.map { it.thinkCtl }.distinctUntilChanged() }
        .collectAsStateWithLifecycle(null)

    if (showSettings) {
        SettingsScreen(
            current = settings,
            onChange = { settings = it; it.save(context) },
            onBack = { showSettings = false },
            thinkCtl = thinkCtl,
        )
    } else if (showMetrics) {
        MetricsScreen(onBack = { showMetrics = false })
    } else {
        MainScreen(
            settings = settings,
            models = models,
            scanning = scanning,
            modelIdx = modelIdx.coerceIn(0, maxOf(0, models.size - 1)),
            onSelectModel = { modelIdx = it },
            onRefresh = { refreshKey++ },
            onOpenSettings = { showSettings = true },
            onOpenMetrics = { showMetrics = true },
        )
    }
}

@Composable
private fun MainScreen(
    settings: AppSettings,
    models: List<File>,
    scanning: Boolean,
    modelIdx: Int,
    onSelectModel: (Int) -> Unit,
    onRefresh: () -> Unit,
    onOpenSettings: () -> Unit,
    onOpenMetrics: () -> Unit,
) {
    val context = LocalContext.current
    val focusManager = LocalFocusManager.current
    val ui by RunBus.state.collectAsStateWithLifecycle()

    var prompt by rememberSaveable { mutableStateOf("Explain what a mixture-of-experts model is, in two sentences.") }
    val listState = rememberLazyListState()

    // Item 0 is the controls block; the transcript and the in-flight answer follow it. The live
    // turn also shows while only reasoning has streamed (the thinking phase, before any answer),
    // so a Thinking-on run does not sit on a blank screen while the model reasons.
    val liveShown = ui.answer.isNotEmpty() || ui.reasoning.isNotEmpty()
    val total = 1 + ui.transcript.size + (if (liveShown) 1 else 0)

    // Follow the tail only while the user is parked at the bottom. A long answer streams for a
    // long time, and scrolling back to re-read it must not fight a per-token scroll command:
    // dragging the list detaches the follow, coming back to the bottom re-arms it.
    var followTail by remember { mutableStateOf(true) }
    val atBottom by remember {
        derivedStateOf {
            val info = listState.layoutInfo
            val last = info.visibleItemsInfo.lastOrNull()
            last == null ||
                (last.index == info.totalItemsCount - 1 && last.offset + last.size <= info.viewportEndOffset)
        }
    }
    LaunchedEffect(listState) {
        listState.interactionSource.interactions.collect { if (it is DragInteraction.Start) followTail = false }
    }
    // Re-arm on settle, not the moment the bottom is touched: while an answer streams the bottom
    // keeps moving away, so only where a scroll actually comes to rest says what the user wants.
    LaunchedEffect(listState) {
        snapshotFlow { listState.isScrollInProgress }.collect { scrolling -> if (!scrolling) followTail = atBottom }
    }
    LaunchedEffect(total, ui.answer.length, ui.reasoning.length, followTail) {
        // A long answer is taller than the viewport, so aligning the item's top would park the view
        // on its beginning; the large offset pins the list to the newest text instead.
        if (followTail && total > 1) runCatching { listState.scrollToItem(total - 1, Int.MAX_VALUE) }
    }

    // adjustResize handles the legacy path; imePadding covers edge-to-edge (Android 15+), where the
    // window no longer shrinks for the keyboard. Without it the streaming answer draws behind the IME.
    Box(Modifier.fillMaxSize().imePadding()) {
        LazyColumn(
            state = listState,
            modifier = Modifier.fillMaxSize().padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            item(key = "controls") {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        Text("BigMoeOnEdge", fontSize = 22.sp, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
                        TextButton(onClick = onOpenMetrics) { Text("Metrics") }
                        TextButton(onClick = onOpenSettings) { Text("Settings") }
                    }

                    when {
                        scanning -> Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                            CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                            Text("Scanning for MoE models…", fontSize = 14.sp)
                        }
                        models.isEmpty() -> {
                            ElevatedCard {
                                Text(
                                    ModelManager.pushHint(),
                                    Modifier.padding(12.dp),
                                    fontSize = 13.sp,
                                    fontFamily = FontFamily.Monospace,
                                )
                            }
                            TextButton(onClick = { requestSharedStorageAccess(context); onRefresh() }) { Text("Refresh") }
                        }
                        else -> LabeledDropdown(
                            label = "Model",
                            options = models.map { it.name },
                            selected = modelIdx,
                            onSelect = onSelectModel,
                        )
                    }

                    // Bring a model onto the device without adb: the built-in catalog, an arbitrary
                    // URL, or a local file. All land in the app models dir; on completion we
                    // re-scan so the model appears above.
                    AddModelSection(
                        models = models,
                        scanning = scanning,
                        loadedSig = ui.sessionSig,
                        onModelReady = onRefresh,
                    )

                    OutlinedTextField(
                        value = prompt,
                        onValueChange = { prompt = it },
                        label = { Text("Prompt") },
                        modifier = Modifier.fillMaxWidth(),
                        minLines = 2,
                    )

                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        Button(
                            onClick = {
                                // Drop focus so the soft keyboard retracts: the answer streams into the
                                // space it was covering, and there is otherwise no in-app way to dismiss it.
                                focusManager.clearFocus()
                                if (models.isNotEmpty()) {
                                    // First message of a conversation clears the KV; a follow-up continues it.
                                    launchPrompt(context, models[modelIdx.coerceIn(0, models.size - 1)],
                                        prompt.ifBlank { "The capital of Japan is" }, settings, ui.sessionSig,
                                        clearKv = ui.transcript.isEmpty())
                                }
                            },
                            enabled = !ui.busy && models.isNotEmpty(),
                            modifier = Modifier.weight(1f),
                        ) { Text(if (ui.transcript.isNotEmpty()) "Send" else if (ui.ready) "Send" else "Run") }

                        OutlinedButton(
                            onClick = {
                                context.startService(
                                    Intent(context, RunService::class.java).setAction(RunService.ACTION_CANCEL)
                                )
                            },
                            enabled = ui.generating,
                            modifier = Modifier.weight(1f),
                        ) { Text("Stop") }
                    }

                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        // Start a new conversation: the next Send clears the KV. Keeps the model loaded.
                        TextButton(
                            onClick = { RunBus.update { it.copy(transcript = emptyList(), answer = "", summary = "", error = null) } },
                            enabled = ui.transcript.isNotEmpty() && !ui.busy,
                        ) { Text("New chat") }

                        // The session keeps the model resident (and the cache warm) between prompts. Free it
                        // explicitly, or let the service auto-unload after an idle timeout.
                        if (ui.ready || ui.loading) {
                            TextButton(onClick = {
                                context.startService(
                                    Intent(context, RunService::class.java).setAction(RunService.ACTION_SHUTDOWN)
                                )
                            }) { Text("Unload model") }
                        }
                    }

                    // A quick reminder of the active config (full controls in Settings).
                    Text(configSummary(settings), fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)

                    if (ui.loading) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(12.dp),
                        ) {
                            CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                            Text("Loading model…", fontSize = 14.sp)
                        }
                    }
                    // After the model is loaded, the prompt is prefilled before the first token streams
                    // (no BMOE_PROGRESS yet). Signal that phase so a slow prefill does not look stuck.
                    if (ui.generating && ui.telemetry.step == 0) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(12.dp),
                        ) {
                            CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                            Text("Prefilling prompt…", fontSize = 14.sp)
                        }
                    }

                    TelemetryCard(ui, settings.threads, settings.overlap, settings.ioThreads)
                }
            }

            // Committed turns.
            items(ui.transcript.size) { i -> TurnView(ui.transcript[i]) }

            // The in-flight assistant answer as it streams (its user turn is already in the transcript).
            // While generating, keep the thinking block open so the reasoning is visible as it arrives.
            if (liveShown) {
                item(key = "live") {
                    TurnView(ChatTurn("assistant", ui.answer, reasoning = ui.reasoning), reasoningExpanded = true)
                }
            }
        }

        // Once the follow is detached, the way back to a still-growing answer is a long drag.
        if (!followTail && !atBottom) {
            val scope = rememberCoroutineScope()
            FilledTonalButton(
                onClick = { scope.launch { listState.animateScrollToItem(total - 1, Int.MAX_VALUE) } },
                modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 24.dp),
            ) { Text("Jump to latest") }
        }
    }
}

/**
 * One transcript bubble: a small role label and the message, with an optional metrics line and,
 * for a reasoning model, a collapsible thinking block above the answer. [reasoningExpanded] seeds
 * the block open (used for the in-flight turn, so the reasoning is visible as it streams); committed
 * turns default it closed so the transcript stays readable.
 */
@Composable
private fun TurnView(turn: ChatTurn, reasoningExpanded: Boolean = false) {
    val isUser = turn.role == "user"
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text(
            if (isUser) "You" else "Assistant",
            fontSize = 12.sp, fontWeight = FontWeight.Bold,
            color = if (isUser) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.tertiary,
        )
        if (turn.reasoning.isNotEmpty()) ReasoningBlock(turn.reasoning, reasoningExpanded)
        // The user's own prompt is echoed verbatim; only the model's answer is read as Markdown.
        SelectionContainer {
            if (isUser) Text(turn.text, fontSize = 15.sp) else MarkdownText(turn.text)
        }
        if (turn.metrics.isNotEmpty()) {
            Text(turn.metrics, fontFamily = FontFamily.Monospace, fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/**
 * The model's internal reasoning, rendered as a dimmed, collapsible block distinct from the answer.
 * A thinking model spends its first tokens here; surfacing it (instead of dropping it, or worse,
 * letting it leak into the answer) is what makes a Thinking-on run legible while it reasons. Tapping
 * the header toggles it; [initiallyExpanded] is the starting state.
 */
@Composable
private fun ReasoningBlock(reasoning: String, initiallyExpanded: Boolean) {
    var expanded by rememberSaveable { mutableStateOf(initiallyExpanded) }
    Surface(
        color = MaterialTheme.colorScheme.surfaceVariant,
        shape = MaterialTheme.shapes.small,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(horizontal = 10.dp, vertical = 6.dp)) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                modifier = Modifier.fillMaxWidth().clickable { expanded = !expanded },
            ) {
                Text(
                    "Thinking", fontSize = 12.sp, fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    if (expanded) "▾" else "▸", fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (expanded) {
                SelectionContainer {
                    Text(
                        reasoning, fontSize = 13.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 4.dp),
                    )
                }
            }
        }
    }
}

/** One-line reminder of the active configuration, mirroring the key Settings knobs. */
private fun configSummary(s: AppSettings): String {
    val core = if (s.mmap) {
        "mmap baseline (no streaming) · ${s.threads} threads"
    } else {
        val cache = when {
            s.cacheMb == AppSettings.CACHE_AUTO -> if (s.cacheCeilMb > 0) "cache auto≤${s.cacheCeilMb}" else "cache auto"
            s.cacheMb == 0 -> "cache off"
            else -> "cache ${s.cacheMb} MiB"
        }
        "$cache · ${s.ioThreads} lanes${if (s.overlap) " · overlap" else ""} · ${s.threads} threads"
    }
    val topk = if (s.nExpertUsed > 0) " · top-k ${s.nExpertUsed}" else ""
    return "$core$topk · thinking ${if (s.thinking) "on" else "off"} · build ${BuildConfig.GIT_SHA}"
}

/**
 * "Get a model" card: one-tap downloads of the models this engine is measured on ([ModelCatalog]),
 * plus the escape hatches — any gguf URL (WorkManager) or a file the user already has (SAF
 * picker). Everything lands in the app models dir with no permission; [onModelReady] triggers a
 * re-scan so the new model shows up in the picker above.
 *
 * [models] is the current scan result: it is what tells a catalog entry it is already on device.
 */
@Composable
private fun AddModelSection(
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

    val present = remember(models) { models.map { it.name }.toSet() }

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
                        progress = progress[e.fileName],
                        installShown = showInstall == e.fileName,
                        error = rowError?.takeIf { it.first == e.fileName }?.second,
                        onToggleInstall = { showInstall = if (showInstall == e.fileName) null else e.fileName },
                        onDownload = {
                            error = null
                            rowError = null
                            ModelDownloader.enqueue(context, e.url ?: "", e.fileName, e.approxBytes)
                                .onFailure {
                                    rowError = e.fileName to (it.message ?: "download failed to start")
                                }
                        },
                        onCancel = { ModelDownloader.cancel(context, e.fileName) },
                        onDelete = { deleteTarget = e.fileName },
                    )
                }

                // On-device models the catalog does not list — imported via the URL field or the
                // file picker below. They have no catalog row of their own, only a picker entry, so
                // this is the one place to remove them.
                val extraModels = models.filter { m -> ModelCatalog.entries.none { it.fileName == m.name } }
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
                // A pasted URL has no catalog row to show its progress in.
                progress.filterKeys { k -> ModelCatalog.entries.none { it.fileName == k } }
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
        val copies = remember(fname, models) { ModelManager.copiesOf(context, fname) }
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

@Composable
private fun TelemetryCard(ui: UiState, threads: Int, overlap: Boolean, ioThreads: Int) {
    ElevatedCard(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            if (ui.error != null) {
                Text("error", color = MaterialTheme.colorScheme.error, fontWeight = FontWeight.Bold)
                Text(ui.error, fontFamily = FontFamily.Monospace, fontSize = 12.sp)
                // The context-overflow error is recoverable: the session stays loaded, but the
                // conversation is full. Point the user at New chat.
                if ("n_ctx" in ui.error) {
                    Text("Conversation is full — tap New chat to start over.",
                        fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                return@Column
            }
            val t = ui.telemetry
            // Once the run finishes the summary carries the aggregate average; show that as the
            // headline rate. While generating, show the live instantaneous (last-token) rate.
            val done = t.avgTokensPerSecond > 0
            Text(
                if (done) {
                    // t.step is the last token's 1-based index = tokens actually generated (which can be
                    // < t.steps, the n_predict target, when the model stops early on an end-of-text token).
                    String.format(Locale.US, "%.2f tok/s   avg (%d tokens)", t.avgTokensPerSecond, t.step)
                } else {
                    String.format(Locale.US, "%.2f tok/s   (token %d/%d)", t.tokensPerSecond, t.step, t.steps)
                },
                fontWeight = FontWeight.Bold, fontSize = 18.sp,
            )
            if (ui.streaming) {
                // The compute-vs-flash split and cache hit rate only mean anything with the streamer
                // running. Under mmap the model faults in through the OS page cache, invisible here.
                // The split itself — live last token vs run average, and which term is measured
                // rather than residual — is derived in breakdown(); this only draws it.
                val b = breakdown(t, overlap, busyThreads = threads + if (overlap) ioThreads else 0)
                val suffix = if (b.isAverage) " avg" else ""

                // Headline: token time and its inverse, so no mental arithmetic to get tok/s.
                if (b.wallMs > 0.0) {
                    Text(String.format(Locale.US, "%.0f ms/token  →  %.2f tok/s", b.wallMs, 1000.0 / b.wallMs),
                        fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
                }
                MeterRow("compute$suffix", b.computeMs, b.totalMs, MaterialTheme.colorScheme.primary)
                MeterRow("flash wait$suffix", b.flashWaitMs, b.totalMs, MaterialTheme.colorScheme.tertiary)
                MeterRow("cache mgmt$suffix", b.mgmtMs, b.totalMs, MaterialTheme.colorScheme.secondary)

                // Diagnostic line: WHY compute is what it is, plus cache hit. Near 100% busy is
                // genuinely compute-bound, well below means a throttled/preempted core (a frequency
                // cap, a co-resident process). Major faults/token > 0 means dense weights re-faulted
                // from flash inside the decode.
                val hit = if (t.cacheHitPct >= 0) String.format(Locale.US, "hit %.0f%%", t.cacheHitPct) else "hit —"
                val diag = buildString {
                    if (b.cpuBusyPct >= 0.0) {
                        append(String.format(Locale.US, "CPU %.0f%% busy", b.cpuBusyPct))
                        if (b.faultsPerToken >= 0.0) {
                            append(String.format(Locale.US, "  ·  %.0f faults/tok", b.faultsPerToken))
                        }
                        append("  ·  ")
                    }
                    append(hit)
                }
                Text(diag, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (ui.ioMode != null) {
                    Text("I/O ${ui.ioMode}", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                // CPU temperature — live while generating, a proxy for thermal headroom.
                ui.cpuTempC?.let {
                    Text(String.format(Locale.US, "CPU %.1f°C", it), fontSize = 13.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                // End-of-run figures from the summary: prefill rate, time-to-first-token, the flash
                // streamed this turn and the cache footprint. Only meaningful once generation finishes.
                if (done) {
                    if (t.prefillTps > 0 || t.ttftS >= 0) {
                        val prefill = if (t.prefillTps > 0) String.format(Locale.US, "prefill %.1f tok/s", t.prefillTps) else ""
                        val ttft = if (t.ttftS >= 0) String.format(Locale.US, "TTFT %.2fs", t.ttftS) else ""
                        Text(listOf(prefill, ttft).filter { it.isNotEmpty() }.joinToString("   ·   "), fontSize = 13.sp)
                    }
                    if (t.readMib >= 0 || t.cacheResidentMib >= 0) {
                        val streamed = if (t.readMib >= 0) String.format(Locale.US, "streamed %.0f MB", t.readMib) else ""
                        val cache = if (t.cacheResidentMib >= 0)
                            String.format(Locale.US, "cache %.0f/%.0f MiB", t.cacheResidentMib, t.cacheBudgetMib) else ""
                        Text(listOf(streamed, cache).filter { it.isNotEmpty() }.joinToString("   ·   "),
                            fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            } else {
                Text(
                    "mmap baseline — the model is read through the OS page cache, so per-token flash I/O, " +
                        "the compute split and cache hits are not observable in this mode.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (ui.summary.isNotEmpty()) {
                Text(ui.summary, fontFamily = FontFamily.Monospace, fontSize = 11.sp)
            }
        }
    }
}

@Composable
private fun MeterRow(label: String, value: Double, total: Double, color: androidx.compose.ui.graphics.Color) {
    val frac = if (total > 0) (value / total).toFloat().coerceIn(0f, 1f) else 0f
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(label, fontSize = 12.sp, modifier = Modifier.width(72.dp))
        LinearProgressIndicator(
            progress = { frac },
            color = color,
            modifier = Modifier.weight(1f).height(8.dp),
        )
        Text(String.format(Locale.US, "%.0f ms", value), fontSize = 12.sp, modifier = Modifier.width(56.dp))
    }
}

/**
 * Send [prompt] to the engine. If a session is already loaded for this exact model+settings
 * ([currentSig] matches), the prompt just goes to the warm process (no reload, cache intact);
 * otherwise the session is (re)started with this configuration and the prompt runs as soon as it
 * reports ready. Per-prompt options (n_predict, thinking) ride the request, not the session.
 */
private fun launchPrompt(
    context: android.content.Context,
    model: File,
    prompt: String,
    settings: AppSettings,
    currentSig: String?,
    clearKv: Boolean,
) {
    RunBus.resetGeneration()
    val sig = settings.sessionSignature(model.absolutePath)
    if (currentSig == sig) {
        context.startService(
            Intent(context, RunService::class.java)
                .setAction(RunService.ACTION_GENERATE)
                .putExtra(RunService.EXTRA_PROMPT, prompt)
                .putExtra(RunService.EXTRA_NPREDICT, settings.nPredict)
                .putExtra(RunService.EXTRA_THINK, settings.thinking)
                .putExtra(RunService.EXTRA_CLEAR_KV, clearKv)
        )
    } else {
        // A new session starts with an empty KV and a cleared transcript, so its first turn
        // always clears regardless of [clearKv].
        // One CSV per session: the engine holds it open across every turn, so it is opened here,
        // where a session is opened, and nowhere else.
        val csv = if (settings.metricsCsv) AppSettings.newMetricsCsvPath(context) else null
        val argv = ArrayList(settings.sessionArgv(ModelManager.cliPath(context), model.absolutePath, csv))
        ContextCompat.startForegroundService(
            context,
            Intent(context, RunService::class.java)
                .putExtra(RunService.EXTRA_MODEL, model.absolutePath)
                .putStringArrayListExtra(RunService.EXTRA_ARGV, argv)
                .putExtra(RunService.EXTRA_SIG, sig)
                .putExtra(RunService.EXTRA_PROMPT, prompt)
                .putExtra(RunService.EXTRA_NPREDICT, settings.nPredict)
                .putExtra(RunService.EXTRA_THINK, settings.thinking)
                .putExtra(RunService.EXTRA_CLEAR_KV, true)
        )
    }
}
