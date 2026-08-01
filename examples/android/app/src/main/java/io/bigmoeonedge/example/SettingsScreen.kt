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
 * Groups every tunable the engine exposes. Changes are applied to [current] live and
 * persisted by the caller. The layout mirrors the CLI flags one-to-one.
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
                    "Load the model the ordinary way, without streaming experts. The baseline to compare against",
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
                    "Experts already in memory cost no read at all, so a bigger cache means less waiting on " +
                        "flash — paid for in memory the rest of the system no longer has. Auto sizes it once " +
                        "at load from what is free, then holds that budget. " +
                        if (AppSettings.cacheNeedsForce(current.cacheMb))
                            "The smallest rungs sit below the engine's own floor: a cache too small to hold " +
                                "one token's experts evicts them before they are reused, and only churns."
                        else "The smallest rungs sit below the engine's floor and have to be forced.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                IntSetting(
                    "Auto cache ceiling (MiB)", AppSettings.CACHE_CEIL_CHOICES, current.cacheCeilMb,
                    format = { if (it == 0) "no cap" else "$it MiB" },
                    enabled = stream && current.cacheMb == AppSettings.CACHE_AUTO,
                ) { onChange(current.copy(cacheCeilMb = it)) }
                Text(
                    "Caps what Auto may claim. The system reports the model's own mapped weights as free " +
                        "memory, so left uncapped Auto can ask for more than really exists.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                IntSetting("Parallel I/O lanes", AppSettings.IO_CHOICES, current.ioThreads, enabled = stream) {
                    onChange(current.copy(ioThreads = it))
                }
                Text(
                    "How many expert reads are in flight at once. More lanes help only until the flash itself is saturated.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                SwitchRow(
                    "Direct I/O (O_DIRECT)",
                    "Read experts straight from flash, so the system does not keep a second copy of what the cache above already holds. Falls back automatically where unsupported",
                    current.oDirect, enabled = stream,
                ) { onChange(current.copy(oDirect = it)) }
                SwitchRow(
                    "I/O–compute overlap",
                    "Start the next reads while the current layer is still computing, so waiting for flash happens behind the work instead of after it",
                    current.overlap, enabled = stream,
                ) { onChange(current.copy(overlap = it)) }
                LabeledDropdown(
                    "Dense weights",
                    DenseWeights.values().map { it.label },
                    current.denseWeights.ordinal,
                    enabled = stream,
                ) { onChange(current.copy(denseWeights = DenseWeights.values()[it])) }
                Text(
                    current.denseWeights.blurb,
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                IntSetting(
                    "Temporal prefetch (layers)", AppSettings.PREFETCH_CHOICES, current.prefetchLayers,
                    format = { if (it == 0) "off" else "$it" },
                    // Mutually exclusive with predictive prefetch: two predictors would speculate
                    // the same future twice, and the engine refuses the pair.
                    enabled = stream && cacheOn && !current.predictPrefetch && current.routeAhead == 0,
                ) { onChange(current.copy(prefetchLayers = it)) }
                Text(
                    "Experimental. Bets a layer will reuse the experts it picked for the previous token, and " +
                        "reads them ahead on lanes that would otherwise sit idle. Needs the cache.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                SwitchRow(
                    "Predictive prefetch (experimental)",
                    "Instead of betting on the previous token, asks the next layer's own router one " +
                        "layer early to find out which experts it will actually want. Reads ahead within " +
                        "the budget below and keeps what the prediction names. Needs the cache; replaces " +
                        "temporal prefetch.",
                    current.predictPrefetch,
                    enabled = stream && cacheOn && current.prefetchLayers == 0 && current.routeAhead == 0,
                ) { onChange(current.copy(predictPrefetch = it)) }
                if (current.predictPrefetch) {
                    IntSetting(
                        "Predicted misses to read ahead", AppSettings.PREDICT_SPEC_CHOICES, current.predictSpecMax,
                        format = {
                            when (it) {
                                0 -> "0 — retention only (default)"
                                else -> "$it"
                            }
                        },
                        enabled = stream && cacheOn,
                    ) { onChange(current.copy(predictSpecMax = it)) }
                    Text(
                        "How much reading ahead the prediction may pay for. At zero it reads nothing and only " +
                            "keeps the experts it names from being evicted — the safe setting, since reading " +
                            "ahead competes for the same flash the current token is waiting on.",
                        fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            Section("Speed / quality") {
                // Speculation sits at the top of this section because it is the only entry here that
                // does NOT trade quality: it changes how many tokens a decode confirms, not what the
                // model computes. Not gated on the streamer — it is a decode-loop change, so it
                // applies to the mmap baseline too.
                LabeledDropdown(
                    "Guess ahead",
                    listOf("Off", "Model's own head (MTP)", "Repeated text (n-gram)"),
                    AppSettings.SPEC_CHOICES.indexOf(current.spec).coerceAtLeast(0),
                ) { onChange(current.copy(spec = AppSettings.SPEC_CHOICES[it])) }
                Text(
                    "Guess the next few tokens, then check the whole group in one pass and keep only what " +
                        "the model itself would have produced. Nothing is skipped or approximated. The gain " +
                        "is reading the weights once for several tokens instead of once each; the cost is " +
                        "that checking several at a time makes every layer touch more experts.\n\n" +
                        "The head is a small extra part of the model trained to guess — accurate, but only " +
                        "some models carry it, and running it costs a pass of its own. The n-gram guess " +
                        "instead looks for the text repeating itself, which costs nothing at all and works " +
                        "on every model, but only has something to say when the model is quoting or " +
                        "editing. When it has nothing, that token runs exactly as if this were off.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                if (current.spec != AppSettings.SPEC_OFF) {
                    IntSetting(
                        "Tokens guessed per pass", AppSettings.MTP_DRAFT_CHOICES, current.mtpDraft,
                    ) { onChange(current.copy(mtpDraft = it)) }
                    Text(
                        "How far ahead to guess. Further means more tokens confirmed per pass, but the " +
                            "guesses grow less reliable and a wrong one is paid for and thrown away. " +
                            "The best setting is rarely the largest.",
                        fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                if (current.spec == AppSettings.SPEC_MTP) {
                    IntSetting(
                        "Guess only when confident", AppSettings.MTP_P_MIN_CHOICES, current.mtpPMinPct,
                        format = { if (it == 0) "Always guess" else "Above $it%" },
                    ) { onChange(current.copy(mtpPMinPct = it)) }
                    Text(
                        "Stop guessing as soon as the model is unsure, instead of always filling the pass. " +
                            "A guess not made costs nothing and keeps the pass narrow, so fewer experts have " +
                            "to be read — but it also gives up the tokens that guess might have won.",
                        fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
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
                    "Consult fewer experts per token than the model asks for. Cuts both the computing and " +
                        "the reading, and changes the reply — a deliberate trade of quality for speed.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                IntSetting(
                    "Route-ahead (layers)", AppSettings.ROUTE_AHEAD_CHOICES, current.routeAhead,
                    format = { if (it == 0) "off" else "$it" },
                    // Excludes both prefetchers (the engine refuses speculating a future this has
                    // already fixed) and guessing ahead, where a verify pass is several tokens wide
                    // and this declines to commit on all of them while still paying for itself.
                    // Needs streaming; the cache is what turns it into early reads.
                    enabled = !current.mmap && current.prefetchLayers == 0 && !current.predictPrefetch &&
                        current.spec == AppSettings.SPEC_OFF,
                ) { onChange(current.copy(routeAhead = it)) }
                Text(
                    "Experimental, changes the output. Each layer's expert choice is committed N layers " +
                        "early — so with the cache on their reads start that early and are never wasted. " +
                        "~20% of choices differ from the router's at 1 layer; quality held in the host A/B.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                IntSetting(
                    "Drop cold experts (% of even share)", AppSettings.DROP_COLD_CHOICES, current.dropColdPct,
                    // The rung labels carry the trade, so the blurb below does not have to repeat
                    // it: which value to pick is the only real question this setting poses.
                    format = {
                        when (it) {
                            0 -> "off"
                            50 -> "50% — barely bites"
                            75 -> "75% — recommended"
                            100 -> "100% — fastest, roughest"
                            else -> "$it%"
                        }
                    },
                    // Unlike top-k, this one asks the expert source what is resident, so it needs
                    // both the streamer and a live cache — the same condition prefetch is under.
                    enabled = !current.mmap &&
                        (current.cacheMb == AppSettings.CACHE_AUTO || current.cacheMb > 0),
                ) { onChange(current.copy(dropColdPct = it)) }
                Text(
                    "Waiting for an expert that is not already in memory is what slows a token down. This " +
                        "skips one — but only when it is missing AND the router barely wanted it, below the " +
                        "chosen share of an even split. Experts already in memory always run, and the " +
                        "strongest is never skipped, so quality is spent only where it buys back a read." +
                        "\n\nThe reply changes, and unlike Active experts not the same way twice: what gets " +
                        "skipped depends on what the cache happened to be holding.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                // The threshold is a share of 1/top-k, so a narrow routing changes what the same
                // percentage means: 75% is "below 9.4%" at eight experts but "below 18.8%" at four.
                // Only shown once a model is loaded and reports its width — guessing would be worse
                // than saying nothing.
                val topk = ui.nExpertUsed
                if (current.dropColdPct > 0 && topk != null && topk in 1..4) {
                    Text(
                        "⚠ This model routes only $topk experts per token, so the same share covers far more " +
                            "of the reply than it does on a model that routes many. Check the answers, or " +
                            "turn this off for this model.",
                        fontSize = 12.sp, color = MaterialTheme.colorScheme.error,
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
                Text(
                    "How much of the conversation the session can hold, prompt and reply together. " +
                        "It is also memory: the KV cache is sized for it once when the model opens, " +
                        "and on a model that already fills RAM that memory comes out of the expert " +
                        "cache and the weights. Shorten it on the largest models, raise it when you " +
                        "want long conversations and have the room. Changing it reopens the session.",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            Section("Prompt") {
                SwitchRow(
                    "Thinking",
                    if (thinkingLocked)
                        "This model always reasons — it offers no way to turn thinking off, so the " +
                            "switch is disabled here instead of being ignored silently. Its reasoning " +
                            "still shows in a collapsible block above the reply."
                    else
                        "Let a reasoning model think before answering; its reasoning shows in a " +
                            "collapsible block above the reply. Off tells the model to skip thinking. " +
                            "No effect on models that don't reason.",
                    // Locked reads ON, not OFF: the model reasons on every turn, and that is what
                    // the switch should be showing whatever the stored preference says.
                    checked = current.thinking || thinkingLocked,
                    enabled = !thinkingLocked,
                ) { onChange(current.copy(thinking = it)) }
            }

            Section("Diagnostics") {
                SwitchRow(
                    "Metrics CSV",
                    "Write one CSV per session: every token's timings, its page faults, the cache budget " +
                        "and where memory sat. Takes effect on the next session; share it from the menu",
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
