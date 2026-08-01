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

    if (showSettings) {
        SettingsScreen(
            current = settings,
            onChange = { settings = it; it.save(context) },
            onBack = { showSettings = false },
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

                    // A quick reminder of the active config (full controls in Settings), with the
                    // rest of it one tap away: the line above names the levers that change the kind
                    // of run, but "what exactly was this answer produced under" is a question the
                    // main screen has to be able to answer too, not only a saved CSV (#136).
                    // Remembered, not recomputed: this item redraws on every streamed token (it holds
                    // the telemetry card), and the configuration it describes changes only when the
                    // user changes a setting.
                    val summary = remember(settings) { configSummary(settings) }
                    Text(summary, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    var showConfig by rememberSaveable { mutableStateOf(false) }
                    val flags = remember(settings, models, modelIdx) {
                        models.getOrNull(modelIdx.coerceIn(0, (models.size - 1).coerceAtLeast(0)))
                            ?.let { configFlags(settings, it.absolutePath, settings.metricsCsv) }
                            .orEmpty()
                    }
                    if (flags.isNotEmpty()) {
                        TextButton(
                            onClick = { showConfig = !showConfig },
                            contentPadding = PaddingValues(horizontal = 4.dp, vertical = 0.dp),
                        ) {
                            Text(
                                if (showConfig) "Hide full configuration"
                                else "Full configuration (${flags.size} flags)",
                                fontSize = 12.sp,
                            )
                        }
                        if (showConfig) {
                            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                                // The engine's own flag names rather than prose labels: this is the
                                // command line the session runs on, and a name that matches the CLI
                                // is what makes a screenshot of it reproducible off-device.
                                flags.forEach { (flag, value) ->
                                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                        Text(
                                            flag, fontSize = 11.sp, fontFamily = FontFamily.Monospace,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                            modifier = Modifier.weight(1f),
                                        )
                                        Text(value, fontSize = 11.sp, fontFamily = FontFamily.Monospace)
                                    }
                                }
                            }
                        }
                    }

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

/**
 * One-line reminder of the active configuration. A glance, not the record — [configFlags] is what
 * states the run in full. What earns a place here is what makes this a different KIND of run from
 * the next one: the lossy levers above all (dropping experts and a narrowed top-k change the
 * ANSWER, not just the speed), then the residency policies that decide where the time goes.
 */
private fun configSummary(s: AppSettings): String {
    val parts = mutableListOf<String>()
    if (s.mmap) {
        parts += "mmap baseline (no streaming)"
    } else {
        parts += when {
            s.cacheMb == AppSettings.CACHE_AUTO -> if (s.cacheCeilMb > 0) "cache auto≤${s.cacheCeilMb}" else "cache auto"
            s.cacheMb == 0 -> "cache off"
            else -> "cache ${s.cacheMb} MiB"
        }
        parts += "${s.ioThreads} lanes"
        if (s.overlap) parts += "overlap"
        if (!s.oDirect) parts += "buffered"
        parts += "dense ${s.denseWeights.flag}"
        // Gated exactly as sessionArgv gates the flags themselves: a lever the CLI will not be told
        // about must not be named here, or the line goes back to describing a run that never ran.
        val cacheOn = s.cacheMb == AppSettings.CACHE_AUTO || s.cacheMb > 0
        if (cacheOn) {
            if (s.prefetchLayers > 0) parts += "prefetch ${s.prefetchLayers}"
            else if (s.predictPrefetch) parts += "predict" + if (s.predictSpecMax > 0) " ${s.predictSpecMax}" else ""
            if (s.dropColdPct > 0) parts += "drop ${s.dropColdPct}%"
        }
    }
    parts += "${s.threads} threads"
    if (s.nExpertUsed > 0) parts += "top-k ${s.nExpertUsed}"
    parts += "thinking ${if (s.thinking) "on" else "off"}"
    parts += "build ${BuildConfig.GIT_SHA}"
    return parts.joinToString(" · ")
}

/**
 * The whole configuration as (flag, value) pairs, read back from the argv these settings would
 * actually open the session with. Deliberately NOT a second hand-kept list beside
 * [AppSettings.sessionArgv]: a knob added there appears here on its own, and a knob the argv gates
 * off (dropping without a cache, prefetch under mmap) is absent here for the same reason it is
 * absent from the run. A curated subset is what left the metrics views unable to state their own
 * drop fraction (#136); this display starts out unable to drift.
 */
private fun configFlags(s: AppSettings, modelPath: String, csv: Boolean): List<Pair<String, String>> {
    // Placeholders for the two paths the argv needs but that say nothing about the configuration;
    // the CSV one only has to be non-null for --csv to be emitted at all.
    val argv = s.sessionArgv("bmoe-cli", modelPath, if (csv) "metrics.csv" else null)
    val out = mutableListOf<Pair<String, String>>()
    var i = 1 // argv[0] is the binary
    while (i < argv.size) {
        val flag = argv[i]
        val next = argv.getOrNull(i + 1)
        // Every value here is a path, a number or a keyword — none of them start with a dash, so
        // the next token being one is what distinguishes a value from the following flag.
        if (next != null && !next.startsWith("-")) {
            // Paths are the user's own storage layout, not configuration: name the file, not where it lives.
            out += flag to (if (flag == "-m" || flag == "--csv") File(next).name else next)
            i += 2
        } else {
            out += flag to "on"
            i += 1
        }
    }
    return out
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
