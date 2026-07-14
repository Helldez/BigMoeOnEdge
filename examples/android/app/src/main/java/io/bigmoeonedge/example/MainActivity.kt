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
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
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

    if (showSettings) {
        SettingsScreen(
            current = settings,
            onChange = { settings = it; it.save(context) },
            onBack = { showSettings = false },
        )
    } else {
        MainScreen(
            settings = settings,
            models = models,
            scanning = scanning,
            modelIdx = modelIdx.coerceIn(0, maxOf(0, models.size - 1)),
            onSelectModel = { modelIdx = it },
            onRefresh = { refreshKey++ },
            onOpenSettings = { showSettings = true },
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
) {
    val context = LocalContext.current
    val ui by RunBus.state.collectAsStateWithLifecycle()

    var prompt by rememberSaveable { mutableStateOf("Explain what a mixture-of-experts model is, in two sentences.") }
    val listState = rememberLazyListState()

    // Keep the newest content in view as the answer streams in and turns commit. Item 0 is the
    // controls block; the transcript and the in-flight answer follow it.
    val liveShown = ui.answer.isNotEmpty()
    val total = 1 + ui.transcript.size + (if (liveShown) 1 else 0)
    LaunchedEffect(ui.transcript.size, liveShown, ui.answer.length) {
        if (total > 1) runCatching { listState.animateScrollToItem(total - 1) }
    }

    LazyColumn(
        state = listState,
        modifier = Modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item(key = "controls") {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Text("BigMoeOnEdge", fontSize = 22.sp, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
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

                // Bring a model onto the device without adb: download by URL or pick a local file.
                // Both land in the app models dir; on completion we re-scan so it appears above.
                AddModelSection(onModelReady = onRefresh)

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

                TelemetryCard(ui)
            }
        }

        // Committed turns.
        items(ui.transcript.size) { i -> TurnView(ui.transcript[i]) }

        // The in-flight assistant answer as it streams (its user turn is already in the transcript).
        if (liveShown) {
            item(key = "live") {
                TurnView(ChatTurn("assistant", ui.answer))
            }
        }
    }
}

/** One transcript bubble: a small role label and the message, with an optional metrics line. */
@Composable
private fun TurnView(turn: ChatTurn) {
    val isUser = turn.role == "user"
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text(
            if (isUser) "You" else "Assistant",
            fontSize = 12.sp, fontWeight = FontWeight.Bold,
            color = if (isUser) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.tertiary,
        )
        SelectionContainer { Text(turn.text, fontSize = 15.sp) }
        if (turn.metrics.isNotEmpty()) {
            Text(turn.metrics, fontFamily = FontFamily.Monospace, fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
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
 * "Add a model" card: download a gguf by URL (DownloadManager) or import one the user already
 * has via the system file picker (SAF). Both write to the app models dir with no permission;
 * [onModelReady] triggers a re-scan so the new model shows up in the picker above.
 */
@Composable
private fun AddModelSection(onModelReady: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var url by rememberSaveable { mutableStateOf("") }
    var expanded by rememberSaveable { mutableStateOf(false) }
    var downloadId by rememberSaveable { mutableStateOf(-1L) }
    var progress by remember { mutableStateOf<ModelDownloader.Progress?>(null) }
    var importStatus by remember { mutableStateOf<String?>(null) }
    var importFrac by remember { mutableStateOf(-1f) }
    var error by remember { mutableStateOf<String?>(null) }

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

    // Poll an active download to completion, then finalize (.part → .gguf) and re-scan.
    LaunchedEffect(downloadId) {
        if (downloadId < 0) return@LaunchedEffect
        while (true) {
            val p = ModelDownloader.query(context, downloadId) ?: break
            progress = p
            if (p.state == ModelDownloader.State.SUCCESS) {
                ModelDownloader.finalizeDownload(context, p.name)
                downloadId = -1L; progress = null; onModelReady()
                break
            }
            if (p.state == ModelDownloader.State.FAILED) {
                error = p.reason ?: "download failed"
                downloadId = -1L; progress = null
                break
            }
            delay(700)
        }
    }

    ElevatedCard(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Add a model", fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                TextButton(onClick = { expanded = !expanded }) { Text(if (expanded) "Hide" else "Add") }
            }
            if (expanded) {
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
                                .onSuccess { downloadId = it }
                                .onFailure { error = it.message ?: "invalid URL" }
                        },
                        enabled = url.isNotBlank() && downloadId < 0,
                        modifier = Modifier.weight(1f),
                    ) { Text("Download") }
                    OutlinedButton(
                        onClick = { picker.launch(arrayOf("*/*")) },
                        modifier = Modifier.weight(1f),
                    ) { Text("Pick file") }
                }
            }

            progress?.let { p ->
                val pct = if (p.totalBytes > 0) {
                    (p.downloadedBytes.toFloat() / p.totalBytes).coerceIn(0f, 1f)
                } else {
                    null
                }
                Text(
                    if (pct != null) {
                        String.format(
                            Locale.US, "Downloading %s — %d%% (%d/%d MiB)",
                            p.name, (pct * 100).toInt(), p.downloadedBytes shr 20, p.totalBytes shr 20,
                        )
                    } else {
                        "Downloading ${p.name}…"
                    },
                    fontSize = 12.sp,
                )
                if (pct != null) {
                    LinearProgressIndicator(progress = { pct }, modifier = Modifier.fillMaxWidth())
                } else {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                }
                TextButton(onClick = {
                    ModelDownloader.cancel(context, p.id, p.name)
                    downloadId = -1L; progress = null
                }) { Text("Cancel") }
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
}

@Composable
private fun TelemetryCard(ui: UiState) {
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
                    String.format(Locale.US, "%.2f tok/s   avg (%d tokens)", t.avgTokensPerSecond, t.steps)
                } else {
                    String.format(Locale.US, "%.2f tok/s   (token %d/%d)", t.tokensPerSecond, t.step, t.steps)
                },
                fontWeight = FontWeight.Bold, fontSize = 18.sp,
            )
            if (ui.streaming) {
                // The compute-vs-flash split and cache hit rate only mean anything with the streamer
                // running. Under mmap the model faults in through the OS page cache, invisible here.
                // Once done, show the per-token AVERAGE split; while generating, the live last token.
                val useAvg = done && t.avgComputeMs >= 0 && t.avgIoMs >= 0
                val compute = if (useAvg) t.avgComputeMs else t.computeMs
                val io = if (useAvg) t.avgIoMs else t.ioMs
                val suffix = if (useAvg) " avg" else ""
                val hit = if (t.cacheHitPct >= 0) String.format(Locale.US, "%.0f%%", t.cacheHitPct) else "—"
                MeterRow("compute$suffix", compute, compute + io, MaterialTheme.colorScheme.primary)
                MeterRow("flash I/O$suffix", io, compute + io, MaterialTheme.colorScheme.tertiary)
                Text("cache hit $hit", fontSize = 13.sp)
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
        val argv = ArrayList(settings.sessionArgv(ModelManager.cliPath(context), model.absolutePath))
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
