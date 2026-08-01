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
 * [ExperimentalGroup]. That is a statement about evidence, not about how finished the code is:
 * inside are the levers measured on one device, or measured once, or still owed a measurement.
 * They stay in the release build because testing them on other hardware is what the demo app is
 * for, and a lever nobody can reach is a lever nobody can refute.
 *
 * Descriptions say what a setting does for the person reading, not how it is implemented, and
 * carry no measured figures. A number here would need the device, the model and the day beside it
 * to mean anything, and none of those fit under a switch. The documents do that job.
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
                    "Load the whole model instead",
                    "Let the system load the model the ordinary way rather than streaming experts " +
                        "from storage. On a model past your memory this is much slower; it is here " +
                        "to compare against.",
                    current.mmap,
                ) { onChange(current.copy(mmap = it)) }

                IntSetting(
                    "Expert cache", AppSettings.CACHE_CHOICES, current.cacheMb,
                    format = {
                        when (it) {
                            AppSettings.CACHE_AUTO -> "Auto"
                            0 -> "Off"
                            else -> "$it MiB"
                        }
                    },
                    enabled = stream,
                ) { onChange(current.copy(cacheMb = it)) }
                Hint(
                    "An expert already in memory costs no read at all, so a bigger cache means less " +
                        "waiting on storage, paid for in memory the rest of the system no longer has. " +
                        "Auto sizes it once when the model opens and then holds that budget. " +
                        "The smallest sizes sit below the engine's own floor: a cache too small to " +
                        "hold what a single token needs throws experts away before they are reused, " +
                        "so it only churns."
                )
                IntSetting(
                    "Cache ceiling for Auto", AppSettings.CACHE_CEIL_CHOICES, current.cacheCeilMb,
                    format = { if (it == 0) "No cap" else "$it MiB" },
                    enabled = stream && current.cacheMb == AppSettings.CACHE_AUTO,
                ) { onChange(current.copy(cacheCeilMb = it)) }
                Hint(
                    "Caps what Auto may claim. The system counts the model's own mapped weights as " +
                        "free memory, so left uncapped Auto can ask for more than really exists."
                )
                IntSetting("Parallel reads", AppSettings.IO_CHOICES, current.ioThreads, enabled = stream) {
                    onChange(current.copy(ioThreads = it))
                }
                Hint(
                    "How many expert reads are in flight at once. More helps only until the storage " +
                        "itself is saturated, and past that point it costs without buying anything."
                )
                SwitchRow(
                    "Read straight from storage",
                    "Skip the system's file cache, so it does not keep a second copy of what the " +
                        "expert cache already holds. Falls back on its own where the storage does " +
                        "not support it.",
                    current.oDirect, enabled = stream,
                ) { onChange(current.copy(oDirect = it)) }
                SwitchRow(
                    "Read while computing",
                    "Start the next reads while the current layer is still being computed, so " +
                        "waiting for storage happens behind the work instead of after it.",
                    current.overlap, enabled = stream,
                ) { onChange(current.copy(overlap = it)) }
                LabeledDropdown(
                    "Always-needed weights",
                    DenseWeights.values().map { it.label },
                    current.denseWeights.ordinal,
                    enabled = stream,
                ) { onChange(current.copy(denseWeights = DenseWeights.values()[it])) }
                Hint(current.denseWeights.blurb)

                ExperimentalGroup {
                    IntSetting(
                        "Read ahead from the last token", AppSettings.PREFETCH_CHOICES, current.prefetchLayers,
                        format = { if (it == 0) "Off" else "$it layers" },
                        // Mutually exclusive with predictive prefetch: two predictors would speculate
                        // the same future twice, and the engine refuses the pair.
                        enabled = stream && cacheOn && !current.predictPrefetch && current.routeAhead == 0,
                    ) { onChange(current.copy(prefetchLayers = it)) }
                    Hint(
                        "Bets that a layer will want the same experts it wanted for the previous " +
                            "token, and fetches them on reads that would otherwise sit idle. Needs " +
                            "the expert cache."
                    )
                    SwitchRow(
                        "Ask the next layer what it wants",
                        "Instead of betting on the previous token, asks the next layer itself which " +
                            "experts it is about to need, and fetches those. Needs the expert cache, " +
                            "and replaces the guess above.",
                        current.predictPrefetch,
                        enabled = stream && cacheOn && current.prefetchLayers == 0 && current.routeAhead == 0,
                    ) { onChange(current.copy(predictPrefetch = it)) }
                    if (current.predictPrefetch) {
                        IntSetting(
                            "How much to fetch on that answer", AppSettings.PREDICT_SPEC_CHOICES,
                            current.predictSpecMax,
                            format = { if (it == 0) "Only keep what it names" else "$it per layer" },
                            enabled = stream && cacheOn,
                        ) { onChange(current.copy(predictSpecMax = it)) }
                        Hint(
                            "At the first setting it fetches nothing and only protects the experts it " +
                                "names from being thrown out, which is the safe choice: fetching ahead " +
                                "competes for the same storage the current token is waiting on."
                        )
                    }
                }
            }

            Section("Speed and quality") {
                // Both entries here are load-time or residency policies, not decode-loop changes.
                IntSetting(
                    "Skip experts that are not in memory", AppSettings.DROP_COLD_CHOICES, current.dropColdPct,
                    format = {
                        when (it) {
                            0 -> "Off"
                            50 -> "Rarely"
                            75 -> "Recommended"
                            100 -> "Fastest, roughest"
                            else -> "$it%"
                        }
                    },
                    // Unlike top-k, this one asks the expert source what is resident, so it needs
                    // both the streamer and a live cache, the same condition prefetch is under.
                    enabled = stream && cacheOn,
                ) { onChange(current.copy(dropColdPct = it)) }
                Hint(
                    "Waiting for an expert that is not already in memory is what slows a token down. " +
                        "This skips one, but only when it is missing and the model barely wanted it. " +
                        "An expert already in memory always runs, and the one the model wanted most " +
                        "is never skipped, so quality is given up only where it buys back a read." +
                        "\n\nThe reply changes, and not the same way twice: what gets skipped depends " +
                        "on what the cache happened to be holding at that moment."
                )
                // The threshold is a share of the even split, so a narrow routing changes what the
                // same setting means. Only shown once a model is loaded and reports its width;
                // guessing would be worse than saying nothing.
                val topk = ui.nExpertUsed
                if (current.dropColdPct > 0 && topk != null && topk in 1..4) {
                    Text(
                        "This model asks for very few experts per token, so the same setting covers " +
                            "far more of the reply than it does on a model that asks for many. Check " +
                            "the answers, or turn this off for this model.",
                        fontSize = 12.sp, color = MaterialTheme.colorScheme.error,
                    )
                }
                IntSetting(
                    "Experts per token", AppSettings.N_EXPERT_CHOICES, current.nExpertUsed,
                    format = { if (it == 0) "As the model asks" else "$it" },
                ) { onChange(current.copy(nExpertUsed = it)) }
                Hint(
                    "Consult fewer experts per token than the model asks for. Cuts the computing and " +
                        "the reading together, and changes the reply: a deliberate trade of quality " +
                        "for speed."
                )

                ExperimentalGroup {
                    LabeledDropdown(
                        "Guess several tokens at once",
                        listOf("Off", "The model's own guess", "Text that repeats"),
                        AppSettings.SPEC_CHOICES.indexOf(current.spec).coerceAtLeast(0),
                        // Excluded by route-ahead, which declines to commit across a wider pass while
                        // still paying for its prediction.
                        enabled = current.routeAhead == 0,
                    ) { onChange(current.copy(spec = AppSettings.SPEC_CHOICES[it])) }
                    Hint(
                        "Guess the next few tokens, then check the whole group in one pass and keep " +
                            "only what the model itself would have produced. Nothing is skipped or " +
                            "approximated, so the reply is the one you would have got anyway. It wins " +
                            "by reading the weights once for several tokens instead of once each, and " +
                            "loses when checking several at a time makes every layer touch more " +
                            "experts than it would have." +
                            "\n\nThe model's own guess comes from a small extra part trained for it: " +
                            "accurate, but only some models carry it and running it costs a pass of " +
                            "its own. The other watches for the text repeating itself, costs nothing " +
                            "and works on every model, but has something to say only when the model " +
                            "is quoting or editing. When it has nothing, that token runs exactly as " +
                            "if this were off."
                    )
                    if (current.spec != AppSettings.SPEC_OFF) {
                        IntSetting(
                            "Tokens per guess", AppSettings.MTP_DRAFT_CHOICES, current.mtpDraft,
                        ) { onChange(current.copy(mtpDraft = it)) }
                        Hint(
                            "How far ahead to guess. Further means more tokens confirmed per pass, but " +
                                "guesses grow less reliable the further out they go, and a wrong one is " +
                                "paid for and thrown away. The best setting is rarely the largest."
                        )
                    }
                    if (current.spec == AppSettings.SPEC_MTP) {
                        IntSetting(
                            "Guess only when sure", AppSettings.MTP_P_MIN_CHOICES, current.mtpPMinPct,
                            format = { if (it == 0) "Always guess" else "Above $it%" },
                        ) { onChange(current.copy(mtpPMinPct = it)) }
                        Hint(
                            "Stop guessing as soon as the model is unsure, instead of always filling " +
                                "the pass. A guess not made costs nothing and keeps the pass narrow, so " +
                                "fewer experts have to be read, but it also gives up the tokens that " +
                                "guess might have won."
                        )
                    }
                    IntSetting(
                        "Decide the experts early", AppSettings.ROUTE_AHEAD_CHOICES, current.routeAhead,
                        format = { if (it == 0) "Off" else "$it layers early" },
                        // Excludes both prefetchers (the engine refuses speculating a future this has
                        // already fixed) and guessing ahead. Needs streaming; the cache is what turns
                        // it into early reads.
                        enabled = stream && current.prefetchLayers == 0 && !current.predictPrefetch &&
                            current.spec == AppSettings.SPEC_OFF,
                    ) { onChange(current.copy(routeAhead = it)) }
                    Hint(
                        "Settles which experts a layer will use before that layer is reached, so their " +
                            "reads can start early and none of them is ever wasted. Changes the reply: " +
                            "some of those choices differ from the ones the model would have made. " +
                            "Cannot be combined with guessing ahead or with the fetching above."
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
                IntSetting("Conversation length", AppSettings.CTX_CHOICES, current.sessionCtx) {
                    onChange(current.copy(sessionCtx = it))
                }
                Hint(
                    "How much of the conversation the model can hold at once, your prompt and its " +
                        "reply together. It is also memory: room for it is set aside when the model " +
                        "opens, and on a model that already fills your device that room comes out of " +
                        "the expert cache. Shorten it on the largest models, raise it when you want " +
                        "long conversations and have the space. Changing it reopens the model."
                )
            }

            Section("Prompt") {
                SwitchRow(
                    "Thinking",
                    if (thinkingLocked)
                        "This model always reasons and offers no way to turn it off, so the switch " +
                            "is disabled here rather than ignored silently. Its reasoning still " +
                            "appears in a block above the reply."
                    else
                        "Let a model that can reason think before answering; its reasoning appears " +
                            "in a block above the reply. Off asks it to answer directly. No effect " +
                            "on models that do not reason.",
                    // Locked reads ON, not OFF: the model reasons on every turn, and that is what
                    // the switch should be showing whatever the stored preference says.
                    checked = current.thinking || thinkingLocked,
                    enabled = !thinkingLocked,
                ) { onChange(current.copy(thinking = it)) }
            }

            Section("Diagnostics") {
                SwitchRow(
                    "Save a metrics file",
                    "Write one file per session with every token's timings, the memory it needed and " +
                        "where the time went. Takes effect the next time a model opens; share it from " +
                        "the menu.",
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
