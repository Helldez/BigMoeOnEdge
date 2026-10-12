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
import androidx.compose.foundation.verticalScroll
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
            val appColors = if (isSystemDark()) {
                darkColorScheme(
                    primary = androidx.compose.ui.graphics.Color(0xFFA6D9C2),
                    onPrimary = androidx.compose.ui.graphics.Color(0xFF003827),
                    secondary = androidx.compose.ui.graphics.Color(0xFFB6C9FF),
                    background = androidx.compose.ui.graphics.Color(0xFF101412),
                    surface = androidx.compose.ui.graphics.Color(0xFF191D1A),
                    surfaceVariant = androidx.compose.ui.graphics.Color(0xFF29302B),
                    onSurface = androidx.compose.ui.graphics.Color(0xFFE1E7E1),
                    onSurfaceVariant = androidx.compose.ui.graphics.Color(0xFFC0C9C0),
                )
            } else {
                lightColorScheme(
                    primary = androidx.compose.ui.graphics.Color(0xFF176B4D),
                    onPrimary = androidx.compose.ui.graphics.Color.White,
                    secondary = androidx.compose.ui.graphics.Color(0xFF355E91),
                    background = androidx.compose.ui.graphics.Color(0xFFF5F7F4),
                    surface = androidx.compose.ui.graphics.Color.White,
                    surfaceVariant = androidx.compose.ui.graphics.Color(0xFFE8EFE9),
                    onSurface = androidx.compose.ui.graphics.Color(0xFF171D19),
                    onSurfaceVariant = androidx.compose.ui.graphics.Color(0xFF424A43),
                )
            }
            MaterialTheme(colorScheme = appColors) {
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
    // Where the NPU prefill cannot run (no Hexagon backend in this APK, or no Snapdragon), a saved
    // switch is read as off, so the settings shown and the argv built always agree.
    val npuUnavailable = remember { ModelManager.npuUnavailableReason(context) }
    var settings by remember {
        mutableStateOf(AppSettings.load(context).let { if (npuUnavailable == null) it else it.copy(npuPrefill = false) })
    }

    // Model-scan state lives here, above the settings/main switch, so opening Settings and
    // coming back does NOT dispose it and trigger a fresh scan. The scan runs once (and again
    // only when refreshKey changes: an explicit Refresh, or after a download/import completes).
    var models by remember { mutableStateOf<List<File>>(emptyList()) }
    var scanning by remember { mutableStateOf(true) }
    var refreshKey by remember { mutableStateOf(0) }
    var modelIdx by remember { mutableStateOf(0) }
    var importedModelPath by remember { mutableStateOf<String?>(null) }

    // Scan model headers off the main thread. After a local import, select that model automatically.
    LaunchedEffect(refreshKey) {
        scanning = true
        val found = withContext(Dispatchers.IO) { ModelManager.listMoeModels(context) }
        models = found
        val importedPath = importedModelPath
        if (importedPath != null) {
            val importedIndex = found.indexOfFirst { it.absolutePath == importedPath }
            if (importedIndex >= 0) modelIdx = importedIndex
            else if (modelIdx >= found.size) modelIdx = 0
            importedModelPath = null
        } else if (modelIdx >= found.size) {
            modelIdx = 0
        }
        scanning = false
    }

    if (showSettings) {
        SettingsScreen(
            current = settings,
            npuUnavailable = npuUnavailable,
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
            onModelImported = { file ->
                importedModelPath = file.absolutePath
                refreshKey++
            },
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
    onModelImported: (File) -> Unit,
    onOpenSettings: () -> Unit,
    onOpenMetrics: () -> Unit,
) {
    val context = LocalContext.current
    val focusManager = LocalFocusManager.current
    val ui by RunBus.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()

    var prompt by rememberSaveable { mutableStateOf("") }
    var choose by rememberSaveable { mutableStateOf(false) }
    var optionsText by rememberSaveable { mutableStateOf("") }
    var showTelemetry by rememberSaveable { mutableStateOf(false) }
    var showConfig by rememberSaveable { mutableStateOf(false) }
    var importing by rememberSaveable { mutableStateOf(false) }
    var importMessage by rememberSaveable { mutableStateOf<String?>(null) }
    var importProgress by remember { mutableStateOf(-1f) }
    val listState = rememberLazyListState()

    // Use the existing SAF importer. It copies the selected GGUF to app-internal storage,
    // where the engine can open a real file path without extra storage permissions.
    val modelPicker = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            importing = true
            importMessage = "正在匯入本機模型…"
            importProgress = -1f
            scope.launch {
                var lastBucket = -1L
                val result = SafImport.importGguf(context, uri) { copied, total ->
                    if (total > 0L) {
                        val bucket = copied / (64L * 1024L * 1024L)
                        if (bucket != lastBucket || copied == total) {
                            lastBucket = bucket
                            importProgress = (copied.toFloat() / total.toFloat()).coerceIn(0f, 1f)
                            importMessage = "匯入中：${copied / (1024L * 1024L)} / ${total / (1024L * 1024L)} MiB"
                        }
                    }
                }
                result.fold(
                    onSuccess = { file ->
                        importing = false
                        importProgress = -1f
                        importMessage = "匯入完成：${file.name}。若清單未出現，可能不是受支援的 MoE 模型。"
                        onModelImported(file)
                    },
                    onFailure = { error ->
                        importing = false
                        importProgress = -1f
                        importMessage = "匯入失敗：${error.message ?: "請確認檔案是有效的 .gguf"}"
                    },
                )
            }
        }
    }

    val liveShown = ui.answer.isNotEmpty() || ui.reasoning.isNotEmpty()
    val hasWelcome = ui.transcript.isEmpty() && !liveShown
    val total = ui.transcript.size + (if (liveShown) 1 else 0) + (if (hasWelcome) 1 else 0)

    // Only the conversation list scrolls. Header, model selector, performance summary,
    // and message composer remain fixed on screen.
    var followTail by remember { mutableStateOf(true) }
    val atBottom by remember {
        derivedStateOf {
            val info = listState.layoutInfo
            val last = info.visibleItemsInfo.lastOrNull()
            last == null || (last.index == info.totalItemsCount - 1 &&
                last.offset + last.size <= info.viewportEndOffset)
        }
    }
    LaunchedEffect(listState) {
        listState.interactionSource.interactions.collect {
            if (it is DragInteraction.Start) followTail = false
        }
    }
    LaunchedEffect(listState) {
        snapshotFlow { listState.isScrollInProgress }.collect { scrolling ->
            if (!scrolling) followTail = atBottom
        }
    }
    LaunchedEffect(total, ui.answer.length, ui.reasoning.length, followTail) {
        if (followTail && total > 0) {
            runCatching { listState.scrollToItem(total - 1, Int.MAX_VALUE) }
        }
    }

    Box(Modifier.fillMaxSize().imePadding()) {
        Column(Modifier.fillMaxSize()) {
            // Fixed top bar.
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                Column(Modifier.weight(1f)) {
                    Text("BigMoe", fontSize = 22.sp, fontWeight = FontWeight.Bold)
                    Text(
                        "本地 AI · 離線推理",
                        fontSize = 11.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                TextButton(
                    onClick = {
                        if (!ui.busy) {
                            RunBus.update {
                                it.copy(transcript = emptyList(), answer = "", reasoning = "", summary = "", error = null)
                            }
                            prompt = ""
                        }
                    },
                    enabled = ui.transcript.isNotEmpty() && !ui.busy,
                    contentPadding = PaddingValues(horizontal = 5.dp, vertical = 4.dp),
                ) { Text("新對話") }
                TextButton(
                    onClick = onOpenMetrics,
                    contentPadding = PaddingValues(horizontal = 5.dp, vertical = 4.dp),
                ) { Text("性能") }
                TextButton(
                    onClick = onOpenSettings,
                    contentPadding = PaddingValues(horizontal = 5.dp, vertical = 4.dp),
                ) { Text("設定") }
            }

            // Model panel: local file selection only. No catalog or URL-download list.
            ElevatedCard(Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 3.dp)) {
                Column(
                    Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(5.dp),
                ) {
                    Row(
                        Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        Column(Modifier.weight(1f)) {
                            Text("模型", fontWeight = FontWeight.SemiBold)
                            Text(
                                when {
                                    scanning -> "正在掃描模型…"
                                    models.isEmpty() -> "尚未載入模型"
                                    else -> models.getOrNull(modelIdx)?.name ?: "選擇模型"
                                },
                                fontSize = 12.sp,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                        Button(
                            onClick = { modelPicker.launch(arrayOf("*/*")) },
                            enabled = !importing && !ui.busy,
                            contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp),
                        ) {
                            Text(if (importing) "匯入中…" else "選擇本機模型")
                        }
                    }

                    when {
                        scanning -> LinearProgressIndicator(Modifier.fillMaxWidth())
                        models.isNotEmpty() -> LabeledDropdown(
                            label = "目前模型",
                            options = models.map { it.name },
                            selected = modelIdx.coerceIn(0, models.lastIndex),
                            onSelect = onSelectModel,
                        )
                        else -> Text(
                            "先下載 .gguf 模型，再點「選擇本機模型」。",
                            fontSize = 12.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }

                    if (importing) {
                        Text(importMessage ?: "正在匯入…", fontSize = 11.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        if (importProgress >= 0f) {
                            LinearProgressIndicator(
                                progress = { importProgress },
                                modifier = Modifier.fillMaxWidth(),
                            )
                        } else {
                            LinearProgressIndicator(Modifier.fillMaxWidth())
                        }
                    } else if (importMessage != null) {
                        Text(
                            importMessage.orEmpty(),
                            fontSize = 11.sp,
                            color = if (importMessage!!.startsWith("匯入失敗"))
                                MaterialTheme.colorScheme.error
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }

                    // SAF import copies the picked model into app storage, so reserve equivalent free space.
                    Text(
                        "本機匯入會複製檔案，請預留與模型大小相同的可用空間。",
                        fontSize = 10.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )

                    if (ui.ready || ui.loading) {
                        TextButton(
                            onClick = {
                                context.startService(
                                    Intent(context, RunService::class.java)
                                        .setAction(RunService.ACTION_SHUTDOWN)
                                )
                            },
                            contentPadding = PaddingValues(horizontal = 0.dp, vertical = 0.dp),
                        ) { Text("卸載模型並釋放記憶體") }
                    }
                }
            }

            // Compact, always-visible live performance strip.
            ElevatedCard(Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 3.dp)) {
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text("生成速度", fontSize = 10.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        val telemetry = ui.telemetry
                        val rate = if (telemetry.avgTokensPerSecond > 0.0 && !ui.streaming)
                            telemetry.avgTokensPerSecond else telemetry.tokensPerSecond
                        Text(
                            if (rate > 0.0) String.format(Locale.US, "%.2f tok/s", rate) else "— tok/s",
                            fontSize = 14.sp,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1,
                        )
                    }
                    Column(Modifier.weight(0.8f)) {
                        Text("CPU 溫度", fontSize = 10.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Text(
                            ui.cpuTempC?.let { String.format(Locale.US, "%.1f°C", it) } ?: "—",
                            fontSize = 14.sp,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1,
                        )
                    }
                    Column(Modifier.weight(0.7f)) {
                        Text("狀態", fontSize = 10.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Text(
                            when {
                                ui.loading -> "載入中"
                                ui.generating -> "生成中"
                                ui.ready -> "已就緒"
                                else -> "待機"
                            },
                            fontSize = 13.sp,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1,
                        )
                    }
                    TextButton(
                        onClick = { showTelemetry = true },
                        contentPadding = PaddingValues(horizontal = 4.dp, vertical = 0.dp),
                    ) { Text("詳情") }
                }
            }

            ui.error?.let {
                Text(
                    it,
                    color = MaterialTheme.colorScheme.error,
                    fontSize = 11.sp,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp),
                )
            }

            // Only this conversation list is vertically scrollable.
            LazyColumn(
                state = listState,
                modifier = Modifier.weight(1f).fillMaxWidth().padding(horizontal = 12.dp),
                contentPadding = PaddingValues(vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                if (hasWelcome) {
                    item(key = "welcome") {
                        Column(
                            Modifier.fillMaxWidth().padding(top = 18.dp, bottom = 12.dp),
                            verticalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            Text("開始一段本機對話", fontSize = 24.sp, fontWeight = FontWeight.Bold)
                            Text(
                                if (models.isEmpty())
                                    "先選擇本機 .gguf 模型，再在下方輸入問題。"
                                else
                                    "模型已就緒。輸入問題，回答會顯示在這裡。",
                                fontSize = 14.sp,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                }
                items(ui.transcript.size) { i -> TurnView(ui.transcript[i]) }
                if (liveShown) {
                    item(key = "live") {
                        TurnView(
                            ChatTurn("assistant", ui.answer, reasoning = ui.reasoning),
                            reasoningExpanded = true,
                        )
                    }
                }
            }

            // Fixed message composer at the bottom.
            Surface(Modifier.fillMaxWidth(), tonalElevation = 2.dp) {
                Column(
                    Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        FilterChip(
                            selected = choose,
                            onClick = { choose = !choose },
                            enabled = !ui.busy,
                            label = { Text("選項模式") },
                        )
                        Spacer(Modifier.weight(1f))
                        if (ui.generating) {
                            Text("正在生成…", fontSize = 11.sp,
                                color = MaterialTheme.colorScheme.primary)
                        }
                    }
                    if (choose) {
                        OutlinedTextField(
                            value = optionsText,
                            onValueChange = { optionsText = it },
                            label = { Text("候選答案，每行一個") },
                            modifier = Modifier.fillMaxWidth(),
                            minLines = 2,
                            maxLines = 3,
                            enabled = !ui.busy,
                        )
                    }
                    Row(
                        verticalAlignment = Alignment.Bottom,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        OutlinedTextField(
                            value = prompt,
                            onValueChange = { prompt = it },
                            placeholder = { Text("傳送訊息給本地模型…") },
                            modifier = Modifier.weight(1f),
                            minLines = 1,
                            maxLines = 4,
                            enabled = !ui.busy,
                        )
                        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Button(
                                onClick = {
                                    focusManager.clearFocus()
                                    val options = if (choose) Choice.parseOptions(optionsText) else null
                                    if (options != null && options.size < 2) {
                                        RunBus.update { it.copy(error = "請至少輸入兩個選項，每行一個。") }
                                    } else if (models.isNotEmpty()) {
                                        launchPrompt(
                                            context,
                                            models[modelIdx.coerceIn(0, models.lastIndex)],
                                            prompt.ifBlank { "請用繁體中文回答。" },
                                            settings,
                                            ui.sessionSig,
                                            clearKv = ui.transcript.isEmpty() || ui.transcript.last().role == "choice",
                                            options = options,
                                        )
                                        prompt = ""
                                    }
                                },
                                enabled = !ui.busy && models.isNotEmpty(),
                            ) { Text("傳送") }
                            if (ui.generating) {
                                OutlinedButton(
                                    onClick = {
                                        context.startService(
                                            Intent(context, RunService::class.java)
                                                .setAction(RunService.ACTION_CANCEL)
                                        )
                                    },
                                ) { Text("停止") }
                            }
                        }
                    }
                }
            }
        }

        if (!followTail && !atBottom && total > 0) {
            val jumpScope = rememberCoroutineScope()
            FilledTonalButton(
                onClick = { jumpScope.launch { listState.animateScrollToItem(total - 1, Int.MAX_VALUE) } },
                modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 106.dp),
            ) { Text("回到最新訊息") }
        }
    }

    // Long telemetry details live in their own scrollable dialog, never expanding the home page.
    if (showTelemetry) {
        val flags = remember(settings, models, modelIdx) {
            models.getOrNull(modelIdx.coerceIn(0, (models.size - 1).coerceAtLeast(0)))
                ?.let { configFlags(settings, it.absolutePath, settings.metricsCsv) }
                .orEmpty()
        }
        AlertDialog(
            onDismissRequest = { showTelemetry = false },
            title = { Text("即時性能詳情") },
            text = {
                Column(
                    Modifier.fillMaxWidth().heightIn(max = 440.dp)
                        .verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    TelemetryCard(ui, settings.threads, settings.overlap, settings.ioThreads)
                    if (flags.isNotEmpty()) {
                        TextButton(
                            onClick = { showConfig = !showConfig },
                            contentPadding = PaddingValues(horizontal = 0.dp, vertical = 0.dp),
                        ) {
                            Text(if (showConfig) "收起完整推理設定" else "查看完整推理設定 (${flags.size})")
                        }
                        if (showConfig) {
                            flags.forEach { (flag, value) ->
                                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                    Text(
                                        flag,
                                        fontSize = 11.sp,
                                        fontFamily = FontFamily.Monospace,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                        modifier = Modifier.weight(1f),
                                    )
                                    Text(value, fontSize = 11.sp, fontFamily = FontFamily.Monospace)
                                }
                            }
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showTelemetry = false }) { Text("完成") }
            },
        )
    }
}

@Composable
private fun TurnView(turn: ChatTurn, reasoningExpanded: Boolean = false) {
    turn.choices?.let { ChoiceView(it, turn.best, turn.metrics); return }
    val isUser = turn.role == "user"
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = if (isUser) Arrangement.End else Arrangement.Start,
    ) {
        Surface(
            modifier = Modifier.fillMaxWidth(if (isUser) 0.88f else 1f),
            shape = MaterialTheme.shapes.large,
            color = if (isUser) MaterialTheme.colorScheme.secondaryContainer
                else MaterialTheme.colorScheme.surface,
            tonalElevation = if (isUser) 0.dp else 1.dp,
        ) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    if (isUser) "你" else "BigMoe",
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    color = if (isUser) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.onSurfaceVariant,
                )
                if (turn.reasoning.isNotEmpty()) ReasoningBlock(turn.reasoning, reasoningExpanded)
                SelectionContainer {
                    if (isUser) Text(turn.text, fontSize = 15.sp) else MarkdownText(turn.text)
                }
                if (turn.metrics.isNotEmpty()) {
                    Text(turn.metrics, fontFamily = FontFamily.Monospace, fontSize = 11.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
    }
}

/**
 * The model's side of a Choose turn: every option with the probability the model put on it, the
 * picked one in bold. The bars need not add up to 100%: the rest is what the model would have said
 * instead of any option, and a large rest is the model saying it is unsure.
 */
@Composable
private fun ChoiceView(choices: List<ChoiceScore>, best: Int, metrics: String) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(
            "Assistant", fontSize = 12.sp, fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.tertiary,
        )
        choices.forEachIndexed { i, c ->
            val picked = i == best
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    "${c.label}) ${c.text}", fontSize = 14.sp,
                    fontWeight = if (picked) FontWeight.Bold else FontWeight.Normal,
                    maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f),
                )
                LinearProgressIndicator(
                    progress = { c.prob.toFloat().coerceIn(0f, 1f) },
                    modifier = Modifier.width(72.dp).height(6.dp),
                )
                Text(
                    String.format(Locale.US, "%.0f%%", 100.0 * c.prob), fontSize = 12.sp,
                    modifier = Modifier.width(40.dp),
                )
            }
        }
        if (metrics.isNotEmpty()) {
            Text(metrics, fontFamily = FontFamily.Monospace, fontSize = 11.sp,
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
            if (s.substitutePct > 0) parts += "prefer-cached ${s.substitutePct}%"
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
                // rather than residual — is derived in breakdown(); this only draws it. The busy
                // denominator counts the I/O lanes; the compute one does not (see breakdown()).
                val b = breakdown(t, overlap,
                    busyThreads = threads + if (overlap) ioThreads else 0, computeThreads = threads)
                val suffix = if (b.isAverage) " avg" else ""

                // Headline: token time and its inverse, so no mental arithmetic to get tok/s.
                if (b.wallMs > 0.0) {
                    Text(String.format(Locale.US, "%.0f ms/token  →  %.2f tok/s", b.wallMs, 1000.0 / b.wallMs),
                        fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
                }
                MeterRow("compute$suffix", b.computeMs, b.totalMs, MaterialTheme.colorScheme.primary)
                MeterRow("flash wait$suffix", b.flashWaitMs, b.totalMs, MaterialTheme.colorScheme.tertiary)
                MeterRow("cache mgmt$suffix", b.mgmtMs, b.totalMs, MaterialTheme.colorScheme.secondary)
                // The fourth bar is the wall time nobody claims: off-CPU cost (zram, preemption,
                // frequency caps) the residual `compute_ms` used to absorb silently. Muted color —
                // it is a question mark, not a workload.
                MeterRow("unattributed$suffix", b.unattributedMs, b.totalMs, MaterialTheme.colorScheme.outline)

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
 * With [options] the request is a Choose turn: the model picks one of them (see [Choice]).
 */
private fun launchPrompt(
    context: android.content.Context,
    model: File,
    prompt: String,
    settings: AppSettings,
    currentSig: String?,
    clearKv: Boolean,
    options: List<String>? = null,
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
                .apply { options?.let { putStringArrayListExtra(RunService.EXTRA_OPTIONS, ArrayList(it)) } }
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
                .apply { options?.let { putStringArrayListExtra(RunService.EXTRA_OPTIONS, ArrayList(it)) } }
        )
    }
}
