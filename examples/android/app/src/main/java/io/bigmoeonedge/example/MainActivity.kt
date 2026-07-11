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
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
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
        requestAllFilesAccess()
        setContent {
            MaterialTheme(colorScheme = if (isSystemDark()) darkColorScheme() else lightColorScheme()) {
                Surface(color = MaterialTheme.colorScheme.background) { Root() }
            }
        }
    }

    // gguf models are read in place from shared storage, which needs all-files access.
    private fun requestAllFilesAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
            runCatching {
                startActivity(
                    Intent(
                        Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:$packageName"),
                    )
                )
            }
        }
    }

    private fun isSystemDark(): Boolean {
        val flag = resources.configuration.uiMode and
            android.content.res.Configuration.UI_MODE_NIGHT_MASK
        return flag == android.content.res.Configuration.UI_MODE_NIGHT_YES
    }
}

@Composable
private fun Root() {
    val context = LocalContext.current
    var showSettings by remember { mutableStateOf(false) }
    var settings by remember { mutableStateOf(AppSettings.load(context)) }

    if (showSettings) {
        SettingsScreen(
            current = settings,
            onChange = { settings = it; it.save(context) },
            onBack = { showSettings = false },
        )
    } else {
        MainScreen(settings = settings, onOpenSettings = { showSettings = true })
    }
}

@Composable
private fun MainScreen(settings: AppSettings, onOpenSettings: () -> Unit) {
    val context = LocalContext.current
    val ui by RunBus.state.collectAsStateWithLifecycle()

    var models by remember { mutableStateOf<List<File>>(emptyList()) }
    var scanning by remember { mutableStateOf(true) }
    var refreshKey by remember { mutableStateOf(0) }
    var modelIdx by remember { mutableStateOf(0) }
    var prompt by rememberSaveable { mutableStateOf("Explain what a mixture-of-experts model is, in two sentences.") }

    // Probing gguf headers to keep only MoE models does blocking reads — off the main thread.
    LaunchedEffect(refreshKey) {
        scanning = true
        models = withContext(Dispatchers.IO) { ModelManager.listMoeModels(context) }
        if (modelIdx >= models.size) modelIdx = 0
        scanning = false
    }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
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
                TextButton(onClick = { refreshKey++ }) { Text("Refresh") }
            }
            else -> LabeledDropdown(
                label = "Model",
                options = models.map { it.name },
                selected = modelIdx.coerceIn(0, models.size - 1),
                onSelect = { modelIdx = it },
            )
        }

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
                        startRun(context, models[modelIdx.coerceIn(0, models.size - 1)],
                            prompt.ifBlank { "The capital of Japan is" }, settings)
                    }
                },
                enabled = !ui.running && models.isNotEmpty(),
                modifier = Modifier.weight(1f),
            ) { Text("Run") }

            OutlinedButton(
                onClick = {
                    context.startService(
                        Intent(context, RunService::class.java).setAction(RunService.ACTION_STOP)
                    )
                },
                enabled = ui.running,
                modifier = Modifier.weight(1f),
            ) { Text("Stop") }
        }

        // A quick reminder of the active streaming config (full controls in Settings).
        Text(
            "cache ${if (settings.cacheMb == 0) "off" else "${settings.cacheMb} MiB"} · " +
                "${settings.ioThreads} lanes · ${settings.threads} threads · " +
                "thinking ${if (settings.thinking) "on" else "off"}",
            fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        if (ui.loading) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                Text("Loading model…", fontSize = 14.sp)
            }
        }

        TelemetryCard(ui)

        if (ui.answer.isNotEmpty()) {
            SelectionContainer { Text(ui.answer, fontSize = 15.sp) }
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
                return@Column
            }
            val t = ui.telemetry
            val hit = if (t.cacheHitPct >= 0) String.format(Locale.US, "%.0f%%", t.cacheHitPct) else "—"
            Text(
                String.format(Locale.US, "%.2f tok/s   (token %d/%d)", t.tokensPerSecond, t.step, t.steps),
                fontWeight = FontWeight.Bold, fontSize = 18.sp,
            )
            MeterRow("compute", t.computeMs, t.computeMs + t.ioMs, MaterialTheme.colorScheme.primary)
            MeterRow("flash I/O", t.ioMs, t.computeMs + t.ioMs, MaterialTheme.colorScheme.tertiary)
            Text("cache hit $hit", fontSize = 13.sp)
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

private fun startRun(context: android.content.Context, model: File, prompt: String, settings: AppSettings) {
    val argv = ArrayList(settings.toArgv(ModelManager.cliPath(context), model.absolutePath, prompt))
    val intent = Intent(context, RunService::class.java)
        .putExtra(RunService.EXTRA_MODEL, model.absolutePath)
        .putExtra(RunService.EXTRA_PROMPT, prompt)
        .putStringArrayListExtra(RunService.EXTRA_ARGV, argv)
    ContextCompat.startForegroundService(context, intent)
    RunBus.reset()
}
