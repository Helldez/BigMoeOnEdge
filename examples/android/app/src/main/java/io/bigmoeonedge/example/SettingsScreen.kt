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
import androidx.lifecycle.compose.collectAsStateWithLifecycle

/**
 * Every tunable the engine exposes, grouped by what it is for. Changes apply to [current] live and
 * the caller persists them.
 *
 * Each category shows the recommended configuration first and folds the rest into an
 * [ExperimentalGroup]: the levers measured on one device, measured once, or still owed a
 * measurement. They stay in the release build because testing them on other hardware is what this
 * app is for, and a lever nobody can reach is a lever nobody can refute.
 *
 * Labels keep the flag's own vocabulary, so a setting here can be matched against the CLI, the CSV
 * preamble and the docs. Descriptions say what the knob does in one or two lines and carry no
 * measured figures: a number would need the device, the model and the day beside it to mean
 * anything, and the docs are where that fits.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(current: AppSettings, onChange: (AppSettings) -> Unit, onBack: () -> Unit) {
    // Reported by the loaded session at BMOE_READY. "none" means this model reasons no matter what
    // it is asked, so the Thinking switch is shown disabled with the reason rather than left there
    // pretending to work (#82). Null = nothing loaded yet, so nothing is claimed either way.
    val ui by RunBus.state.collectAsStateWithLifecycle()
    val thinkingLocked = ui.thinkControl == "none"

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
        val stream = !current.mmap
        val cacheOn = current.cacheMb == AppSettings.CACHE_AUTO || current.cacheMb > 0

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
                    "Load the model the ordinary way. The baseline to compare against.",
                    current.mmap,
                ) { onChange(current.copy(mmap = it)) }

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
                Hint(
                    "A resident expert costs no read, so a bigger cache means less waiting on flash, " +
                        "paid for in RAM. Auto sizes it once at load. The smallest rungs sit below the " +
                        "engine's floor and only churn."
                )
                IntSetting(
                    "Auto cache ceiling (MiB)", AppSettings.CACHE_CEIL_CHOICES, current.cacheCeilMb,
                    format = { if (it == 0) "no cap" else "$it MiB" },
                    enabled = stream && current.cacheMb == AppSettings.CACHE_AUTO,
                ) { onChange(current.copy(cacheCeilMb = it)) }
                Hint(
                    "Caps what Auto may claim. The system counts our own mapped weights as free, so " +
                        "uncapped it can ask for more than exists."
                )
                IntSetting("Parallel I/O lanes", AppSettings.IO_CHOICES, current.ioThreads, enabled = stream) {
                    onChange(current.copy(ioThreads = it))
                }
                Hint("Expert reads in flight at once. Helps only until the flash saturates.")
                SwitchRow(
                    "Direct I/O (O_DIRECT)",
                    "Bypass the page cache, so the system keeps no second copy of what the cache " +
                        "already holds. Falls back where unsupported.",
                    current.oDirect, enabled = stream,
                ) { onChange(current.copy(oDirect = it)) }
                SwitchRow(
                    "I/O and compute overlap",
                    "Issue the next reads while the current layer computes, so flash latency hides " +
                        "behind the work.",
                    current.overlap, enabled = stream,
                ) { onChange(current.copy(overlap = it)) }
                LabeledDropdown(
                    "Dense weights",
                    DenseWeights.values().map { it.label },
                    current.denseWeights.ordinal,
                    enabled = stream,
                ) { onChange(current.copy(denseWeights = DenseWeights.values()[it])) }
                Hint(current.denseWeights.blurb)

                ExperimentalGroup {
                    IntSetting(
                        "Temporal prefetch (layers)", AppSettings.PREFETCH_CHOICES, current.prefetchLayers,
                        format = { if (it == 0) "off" else "$it" },
                        // Mutually exclusive with predictive prefetch: two predictors would speculate
                        // the same future twice, and the engine refuses the pair.
                        enabled = stream && cacheOn && !current.predictPrefetch && current.routeAhead == 0,
                    ) { onChange(current.copy(prefetchLayers = it)) }
                    Hint(
                        "Bets a layer reuses the previous token's experts and reads them on idle lanes. " +
                            "Needs the cache."
                    )
                    SwitchRow(
                        "Predictive prefetch",
                        "Runs the next layer's own router a layer early and prefetches what it names. " +
                            "More accurate than the bet above, and replaces it. Needs the cache.",
                        current.predictPrefetch,
                        enabled = stream && cacheOn && current.prefetchLayers == 0 && current.routeAhead == 0,
                    ) { onChange(current.copy(predictPrefetch = it)) }
                    if (current.predictPrefetch) {
                        IntSetting(
                            "Predicted misses to read ahead", AppSettings.PREDICT_SPEC_CHOICES,
                            current.predictSpecMax,
                            format = { if (it == 0) "retention only" else "$it" },
                            enabled = stream && cacheOn,
                        ) { onChange(current.copy(predictSpecMax = it)) }
                        Hint(
                            "Retention only reads nothing and just protects what the prediction names, " +
                                "which is the safe setting: reading ahead competes with the token now."
                        )
                    }
                }
            }

            Section("Speed / quality") {
                IntSetting(
                    "Drop cold experts (% of even share)", AppSettings.DROP_COLD_CHOICES, current.dropColdPct,
                    format = {
                        when (it) {
                            0 -> "off"
                            50 -> "50% (barely bites)"
                            75 -> "75% (recommended)"
                            100 -> "100% (fastest, roughest)"
                            else -> "$it%"
                        }
                    },
                    // Needs the streamer and a live cache: it asks the source what is resident.
                    enabled = stream && cacheOn,
                ) { onChange(current.copy(dropColdPct = it)) }
                Hint(
                    "Skips a routed expert only when it is a cache miss and the router barely wanted " +
                        "it, so quality is spent only where it buys a read. A resident expert always " +
                        "runs and the top one is never dropped. Changes the reply, and not the same " +
                        "way twice: it depends on what the cache held."
                )
                // The threshold is a share of the even split, so a narrow routing changes what the
                // same percentage means. Only shown once a model reports its width.
                val topk = ui.nExpertUsed
                if (current.dropColdPct > 0 && topk != null && topk in 1..4) {
                    Text(
                        "This model routes very few experts per token, so the same share covers much " +
                            "more of the reply. Check the answers, or turn this off here.",
                        fontSize = 12.sp, color = MaterialTheme.colorScheme.error,
                    )
                }
                IntSetting(
                    "Active experts (top-k)", AppSettings.N_EXPERT_CHOICES, current.nExpertUsed,
                    format = { if (it == 0) "model default" else "$it" },
                ) { onChange(current.copy(nExpertUsed = it)) }
                Hint(
                    "Consult fewer experts per token than the model asks for. Cuts compute and reads " +
                        "together, and changes the reply."
                )

                ExperimentalGroup {
                    LabeledDropdown(
                        "Guess ahead",
                        listOf("Off", "Model's own head (MTP)", "Repeated text (n-gram)"),
                        AppSettings.SPEC_CHOICES.indexOf(current.spec).coerceAtLeast(0),
                        // Excluded by route-ahead, which declines to commit across a wider verify
                        // pass while still paying for its prediction.
                        enabled = current.routeAhead == 0,
                    ) { onChange(current.copy(spec = AppSettings.SPEC_CHOICES[it])) }
                    Hint(
                        "Draft the next few tokens, verify the group in one decode, keep only what the " +
                            "model would have produced. Lossless. Wins by reading the weights once for " +
                            "several tokens, loses when the wider verify makes each layer touch more " +
                            "experts. The head is accurate but only some models carry it; the n-gram " +
                            "lookup is free and works on any model, but only fires on repeated text."
                    )
                    if (current.spec != AppSettings.SPEC_OFF) {
                        IntSetting(
                            "Tokens guessed per pass", AppSettings.MTP_DRAFT_CHOICES, current.mtpDraft,
                        ) { onChange(current.copy(mtpDraft = it)) }
                        Hint(
                            "Wider means more tokens per decode, but drafts get less reliable and a " +
                                "rejected one is paid for anyway. The best value is rarely the largest."
                        )
                    }
                    if (current.spec == AppSettings.SPEC_MTP) {
                        IntSetting(
                            "Guess only when confident", AppSettings.MTP_P_MIN_CHOICES, current.mtpPMinPct,
                            format = { if (it == 0) "always draft" else "above $it%" },
                        ) { onChange(current.copy(mtpPMinPct = it)) }
                        Hint(
                            "Stop drafting once the head is unsure. A draft not made keeps the verify " +
                                "batch narrow, so fewer experts are read."
                        )
                    }
                    IntSetting(
                        "Route-ahead (layers)", AppSettings.ROUTE_AHEAD_CHOICES, current.routeAhead,
                        format = { if (it == 0) "off" else "$it" },
                        // Excludes both prefetchers and speculation; needs the streamer, and the cache
                        // is what turns a committed selection into early reads.
                        enabled = stream && current.prefetchLayers == 0 && !current.predictPrefetch &&
                            current.spec == AppSettings.SPEC_OFF,
                    ) { onChange(current.copy(routeAhead = it)) }
                    Hint(
                        "Commits each layer's routing that many layers early, so its reads start early " +
                            "and can never be wasted. Lossy: some slots route differently. Excludes the " +
                            "prefetchers and Guess ahead."
                    )
                }
            }

            Section("Compute") {
                IntSetting("Compute threads", AppSettings.THREAD_CHOICES, current.threads) {
                    onChange(current.copy(threads = it))
                }
                IntSetting("Tokens to generate", AppSettings.NPREDICT_CHOICES, current.nPredict) {
                    onChange(current.copy(nPredict = it))
                }
                IntSetting("Context (tokens)", AppSettings.CTX_CHOICES, current.sessionCtx) {
                    onChange(current.copy(sessionCtx = it))
                }
                Hint(
                    "Prompt plus reply the session can hold. Also memory: the KV cache is sized for it " +
                        "at open, and on a model that fills RAM that comes out of the expert cache. " +
                        "Changing it reopens the session."
                )
            }

            Section("Prompt") {
                SwitchRow(
                    "Thinking",
                    if (thinkingLocked)
                        "This model always reasons and offers no way to turn it off, so the switch is " +
                            "disabled rather than ignored. Its reasoning still shows above the reply."
                    else
                        "Let a reasoning model think first; its reasoning shows in a block above the " +
                            "reply. No effect on models that do not reason.",
                    // Locked reads ON, not OFF: the model reasons on every turn, and that is what
                    // the switch should be showing whatever the stored preference says.
                    checked = current.thinking || thinkingLocked,
                    enabled = !thinkingLocked,
                ) { onChange(current.copy(thinking = it)) }
            }

            Section("Diagnostics") {
                SwitchRow(
                    "Metrics CSV",
                    "One CSV per session: per-token timings, faults, cache budget and where memory sat. " +
                        "Takes effect on the next session; share it from the menu.",
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
