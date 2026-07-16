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
                val cacheOn = current.cacheMb == AppSettings.CACHE_AUTO || current.cacheMb > 0
                IntSetting(
                    "Expert cache (MiB)", AppSettings.CACHE_CHOICES, current.cacheMb,
                    format = {
                        when (it) {
                            AppSettings.CACHE_AUTO -> "Auto"
                            0 -> "off"
                            else -> "$it MiB"
                        }
                    },
                    enabled = stream,
                ) { onChange(current.copy(cacheMb = it)) }
                Text(
                    "Larger cache = fewer flash reads per token, but more RAM — and RAM the kernel takes " +
                        "back is paid for twice. Auto sizes to free RAM and shrinks under memory pressure. " +
                        if (AppSettings.cacheNeedsForce(current.cacheMb))
                            "500 and 1000 are below the engine's floor (--force-cache): a cache under one " +
                                "token's routed experts can only thrash. Kept for measuring where the cache " +
                                "stops earning its memory."
                        else "500 and 1000 sit below the engine's floor and need --force-cache.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                IntSetting(
                    "Auto cache ceiling (MiB)", AppSettings.CACHE_CEIL_CHOICES, current.cacheCeilMb,
                    format = { if (it == 0) "no cap" else "$it MiB" },
                    enabled = stream && current.cacheMb == AppSettings.CACHE_AUTO,
                ) { onChange(current.copy(cacheCeilMb = it)) }
                Text(
                    "Upper bound on the Auto budget, so it does not grow into memory pressure on " +
                        "devices with tight free RAM.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                SwitchRow(
                    "Pressure-aware cache sizing",
                    "Experimental. Treat the cache budget as a ceiling: the engine watches its own pages, " +
                        "shrinks the cache when the device starts reclaiming it, and grows back when the " +
                        "pressure lifts. Needs the cache on",
                    current.cacheDynamic, enabled = stream && cacheOn,
                ) { onChange(current.copy(cacheDynamic = it)) }
                IntSetting("Parallel I/O lanes", AppSettings.IO_CHOICES, current.ioThreads, enabled = stream) {
                    onChange(current.copy(ioThreads = it))
                }
                Text(
                    "Number of parallel flash-read threads for the expert stream.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                SwitchRow(
                    "Direct I/O (O_DIRECT)",
                    "Bypass the page cache for expert reads. Falls back to buffered automatically if unsupported",
                    current.oDirect, enabled = stream,
                ) { onChange(current.copy(oDirect = it)) }
                SwitchRow(
                    "I/O–compute overlap",
                    "Read the next experts while the current layer computes, hiding read latency",
                    current.overlap, enabled = stream,
                ) { onChange(current.copy(overlap = it)) }
                SwitchRow(
                    "Dense weight warm-up",
                    "Page-cache the non-expert weights at load. Removes the slow first tokens on models larger than RAM",
                    current.warmDense, enabled = stream,
                ) { onChange(current.copy(warmDense = it)) }
                IntSetting(
                    "Temporal prefetch (layers)", AppSettings.PREFETCH_CHOICES, current.prefetchLayers,
                    format = { if (it == 0) "off" else "$it" },
                    enabled = stream && cacheOn,
                ) { onChange(current.copy(prefetchLayers = it)) }
                Text(
                    "Experimental. Prefetch the next K layers' likely experts on idle read lanes. Needs the cache on.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            Section("Speed / quality") {
                // Active-expert (top-k) override is a load-time kv_override, valid in both streaming
                // and mmap mode, so it is not gated on the streamer.
                IntSetting(
                    "Active experts (top-k)", AppSettings.N_EXPERT_CHOICES, current.nExpertUsed,
                    format = {
                        when (it) {
                            0 -> "Model default"
                            else -> "$it"
                        }
                    },
                ) { onChange(current.copy(nExpertUsed = it)) }
                Text(
                    "Route fewer experts per token than the model's default. Faster and lighter on flash, " +
                        "but the output changes — a speed/quality trade-off.",
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

            Section("Diagnostics") {
                SwitchRow(
                    "Metrics CSV",
                    "Write one CSV per session with every token's timings, page faults (count and MiB), " +
                        "cache budget, and the memory split — anon (the expert cache), file (the model), " +
                        "swap. Takes effect on the next session; share it from the menu",
                    current.metricsCsv,
                ) { onChange(current.copy(metricsCsv = it)) }
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
