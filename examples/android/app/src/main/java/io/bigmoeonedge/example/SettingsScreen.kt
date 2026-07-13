package io.bigmoeonedge.example

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * Groups every tunable the engine exposes. Changes are applied to [current] live and
 * persisted by the caller. The layout mirrors the CLI flags one-to-one.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(current: AppSettings, onChange: (AppSettings) -> Unit, onBack: () -> Unit) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Settings") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        Column(
            Modifier
                .padding(padding)
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Section("Streaming") {
                // mmap is the no-streaming baseline. When on, every streaming knob below is
                // inert (the CLI omits --moe-stream and all sub-flags), so they are disabled.
                SwitchRow(
                    "mmap baseline (no streaming)",
                    "Load the whole model via llama.cpp mmap — the baseline to compare against",
                    current.mmap,
                ) { onChange(current.copy(mmap = it)) }

                val stream = !current.mmap
                IntSetting(
                    "Expert cache (MiB)", AppSettings.CACHE_CHOICES, current.cacheMb,
                    format = { if (it == 0) "off" else "$it MiB" },
                    enabled = stream,
                ) { onChange(current.copy(cacheMb = it)) }
                Text(
                    "0 or ≥ 2000 MiB. A smaller cache thrashes (evict + re-read) and is " +
                        "slower than off, so 1000 is intentionally not offered.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                IntSetting("Parallel I/O lanes", AppSettings.IO_CHOICES, current.ioThreads, enabled = stream) {
                    onChange(current.copy(ioThreads = it))
                }
                SwitchRow(
                    "Direct I/O (O_DIRECT)", "Bypass the page cache when reading experts",
                    current.oDirect, enabled = stream,
                ) { onChange(current.copy(oDirect = it)) }
                SwitchRow(
                    "I/O–compute overlap",
                    "Prefetch experts while the layer computes (experimental)",
                    current.overlap, enabled = stream,
                ) { onChange(current.copy(overlap = it)) }
                IntSetting(
                    "Temporal prefetch (layers)", AppSettings.PREFETCH_CHOICES, current.prefetchLayers,
                    format = { if (it == 0) "off" else "$it" },
                    enabled = stream && current.cacheMb > 0,
                ) { onChange(current.copy(prefetchLayers = it)) }
                Text(
                    "Read the next K layers' likely experts on idle lanes, predicted from the " +
                        "previous token. Needs the cache on.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            Section("Compute") {
                IntSetting("Compute threads", AppSettings.THREAD_CHOICES, current.threads) {
                    onChange(current.copy(threads = it))
                }
                IntSetting("Tokens to generate", AppSettings.NPREDICT_CHOICES, current.nPredict) {
                    onChange(current.copy(nPredict = it))
                }
            }

            Section("Prompt") {
                SwitchRow(
                    "Thinking", "Model reasoning; off passes --no-think (works for Qwen and Gemma)",
                    current.thinking,
                ) { onChange(current.copy(thinking = it)) }
            }

            Section("Debug") {
                SwitchRow(
                    "Load all experts", "Read every expert each token (A/B baseline; slow)",
                    current.loadAll, enabled = !current.mmap,
                ) { onChange(current.copy(loadAll = it)) }
            }
        }
    }
}

@Composable
private fun Section(title: String, content: @Composable ColumnScope.() -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(title, fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary)
        content()
    }
}
