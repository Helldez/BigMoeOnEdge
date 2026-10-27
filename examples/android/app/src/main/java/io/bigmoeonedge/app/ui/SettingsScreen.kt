package io.bigmoeonedge.app.ui

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.data.AppPrefs
import io.bigmoeonedge.app.engine.ConfigState
import io.bigmoeonedge.app.engine.ContextWarning
import io.bigmoeonedge.app.engine.contextWarning
import io.bigmoeonedge.app.engine.text
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.engine.ParamSpec
import io.bigmoeonedge.app.engine.LanAddress
import io.bigmoeonedge.app.engine.PlanDecision
import io.bigmoeonedge.app.engine.SettingsCard
import io.bigmoeonedge.app.engine.ValueSource
import io.bigmoeonedge.app.engine.differsFromPlan
import io.bigmoeonedge.app.engine.levelCounts
import io.bigmoeonedge.app.engine.paramValueFromText
import io.bigmoeonedge.app.engine.paramValueText
import io.bigmoeonedge.app.engine.pinnedKeys
import io.bigmoeonedge.app.engine.resettableKeys
import io.bigmoeonedge.app.engine.settingsCards
import io.bigmoeonedge.app.engine.valueSource
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** What a level means, in a line under its fold: the levels are the table's enum, the rows in them the server's. */
private val LEVEL_HINT = mapOf(
    "advanced" to "Measured levers with a known trade-off.",
    "experimental" to "Measured once or on one device: they can slow a run down.",
    "debug" to "Tests, diagnostics and A/B baselines; never a speed setting.",
)

private fun levelName(l: String) = l.replaceFirstChar { it.uppercase() }

/**
 * Settings, rendered from the engine's parameter table (`GET /api/params`) and nothing else: one
 * card per group in the table's order, each with its basic rows open and its deeper levels folded
 * inside it, a level filter and a search over every level (docs/screens.md, "Settings"). The groups,
 * rows, types, bounds and help all come from the engine, so a knob added there appears here with no
 * app change. Values live in the server's layers; a value set here is the person's and a plan never
 * overrides it. [target] is a parameter a link points at (the Plan screen, the context warning):
 * the page lands on that row and calls [onLanded]. The few settings that are this app's own close
 * the page.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(modifier: Modifier = Modifier, target: String = "", onLanded: () -> Unit = {}, onOpenAgent: (AgentPage) -> Unit = {}) {
    val context = LocalContext.current
    val engine by Engine.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }
    var upTo by rememberSaveable { mutableStateOf(AppPrefs.settingsLevel(context, ParamSpec.LEVELS, ParamSpec.LEVELS[1])) }
    var query by rememberSaveable { mutableStateOf("") }
    // Open state per card and per fold, by group: a card with basic rows starts open, one holding only
    // deeper rows starts folded, so the page opens on the recommended configuration.
    var closedCards by remember { mutableStateOf(mapOf<String, Boolean>()) }
    var openFolds by remember { mutableStateOf(mapOf<String, Boolean>()) }
    var landedOn by remember { mutableStateOf("") }
    // A link from inside this screen (the context line under "Serve on the network") lands the same way.
    var jump by remember { mutableStateOf("") }
    val landing = target.ifEmpty { jump }
    var confirmReset by remember { mutableStateOf(false) }

    fun set(key: String, value: Any?) = scope.launch {
        Engine.setConfig(mapOf(key to value)).onFailure { snackbar.showSnackbar(it.message ?: "not saved") }
    }
    fun reset(keys: List<String>) = scope.launch {
        Engine.setConfig(emptyMap(), keys).onFailure { snackbar.showSnackbar(it.message ?: "not reset") }
    }

    val params = engine.params
    val cfg = engine.config
    // A path is set at launch, not over the API: only a read-only group's (chosen on Models) is shown.
    val settable = remember(params) { params?.filter { it.type != "path" || it.groupReadOnly } }

    // A link to one setting lands on it: the filter widened to its level, the search cleared, its card
    // and fold opened; the row itself scrolls into view. Once per link, so editing another field
    // afterwards does not jump back.
    LaunchedEffect(landing, settable, cfg != null) {
        if (landing.isEmpty() || settable == null || cfg == null) return@LaunchedEffect
        val spec = settable.firstOrNull { it.key == landing }
        onLanded()
        jump = ""
        if (spec == null) return@LaunchedEffect
        query = ""
        if (ParamSpec.levelRank(spec.level) > ParamSpec.levelRank(upTo)) upTo = spec.level
        closedCards = closedCards + (spec.group to false)
        if (spec.level != "basic") openFolds = openFolds + (spec.group to true)
        landedOn = spec.key
    }

    Scaffold(
        modifier = modifier,
        topBar = { TopAppBar(title = { Text("Settings") }, navigationIcon = { Box(Modifier.padding(start = 16.dp)) { BrandMark(wordmark = false) } }) },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        Column(
            Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            if (cfg != null && engine.ready && (cfg.reloadRequired || cfg.reloadKeys.isNotEmpty())) {
                ElevatedCard(Modifier.fillMaxWidth()) {
                    Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text("Some changes only take effect after the model is reloaded.", fontSize = 13.sp, modifier = Modifier.weight(1f))
                        TextButton(
                            shape = Corner, enabled = cfg.valid,
                            onClick = { scope.launch { Engine.load(null).onFailure { snackbar.showSnackbar(it.message ?: "reload failed") } } },
                        ) { Text("Reload") }
                    }
                }
            }
            if (cfg != null && !cfg.valid) {
                Text("This configuration does not validate: ${cfg.error}. Loading a model is refused until it is fixed.", color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
            }

            // While the engine restarts (a sharing change, a new key) the rows stay where they are, so the
            // page keeps its length and its scroll; a line above says why a change waits.
            if (!engine.serverUp) {
                Text(
                    if (params != null) "The engine is restarting; changes apply once it is back." else "The engine is not running.",
                    color = if (params != null) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.error,
                    fontSize = 13.sp,
                )
            }
            when {
                settable == null || cfg == null -> if (engine.serverUp) Text("Waiting for the engine…", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                else -> {
                    val searching = query.isNotBlank()
                    val decisions = remember(cfg.plan) { cfg.plan.orEmpty().associateBy { it.knob } }
                    val cards = remember(settable, cfg, upTo, query) { settingsCards(settable, upTo, query, pinnedKeys(settable, cfg)) }
                    val counts = remember(settable) { levelCounts(settable) }
                    val resettable = remember(settable, cfg) { resettableKeys(cfg.userKeys, cfg.launchKeys, settable) }

                    OutlinedTextField(
                        value = query, onValueChange = { query = it },
                        label = { Text("Search settings") }, singleLine = true, modifier = Modifier.fillMaxWidth(),
                    )
                    // Rows up to a level; a row someone or the plan set shows whatever the filter.
                    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        ParamSpec.LEVELS.forEach { l ->
                            FilterChip(
                                selected = ParamSpec.levelRank(l) <= ParamSpec.levelRank(upTo),
                                onClick = { upTo = l; AppPrefs.setSettingsLevel(context, l) },
                                enabled = !searching,
                                shape = Corner,
                                label = { Text("${levelName(l)} ${counts[l] ?: 0}", fontSize = 11.sp, maxLines = 1) },
                            )
                        }
                    }
                    if (searching) Hint(if (cards.isEmpty()) "No setting matches." else "Every level is searched.")
                    if (resettable.isNotEmpty()) {
                        TextButton(shape = Corner, onClick = { confirmReset = true }, contentPadding = PaddingValues(horizontal = 0.dp)) {
                            Text("Reset all my values (${resettable.size})", fontSize = 13.sp)
                        }
                    }
                    if (confirmReset) {
                        AlertDialog(
                            onDismissRequest = { confirmReset = false },
                            title = { Text("Reset all your values?") },
                            text = {
                                Text(
                                    "Your ${resettable.size} ${if (resettable.size == 1) "value goes" else "values go"} back to the engine's " +
                                        "default, or to the plan's where it decided one. What the engine was launched with and the model stay.",
                                    fontSize = 13.sp,
                                )
                            },
                            confirmButton = { TextButton(shape = Corner, onClick = { confirmReset = false; reset(resettable) }) { Text("Reset") } },
                            dismissButton = { TextButton(shape = Corner, onClick = { confirmReset = false }) { Text("Keep") } },
                        )
                    }

                    cards.forEach { c ->
                        val open = searching || !(closedCards[c.group] ?: c.basic.isEmpty())
                        val foldOpen = searching || openFolds[c.group] == true
                        GroupCard(
                            c, open, foldOpen,
                            onToggle = { if (!searching) closedCards = closedCards + (c.group to open) },
                            onToggleFold = { if (!searching) openFolds = openFolds + (c.group to !foldOpen) },
                        ) { spec ->
                            ParamRow(
                                spec, cfg, decisions[spec.key], readOnly = c.readOnly, landing = spec.key == landedOn,
                                onSet = { set(spec.key, it) }, onReset = { reset(listOf(spec.key)) },
                            )
                        }
                    }
                }
            }

            AgentSettingsCard(onOpenAgent)
            HorizontalDivider(color = tokens.line)
            val ctx = contextWarning(engine.info?.model.takeIf { engine.serverUp && engine.info?.ready == true }, params)
            AppSettings(engine.serverUp, ctx) { jump = it }
            engine.info?.let { Text("Engine ${it.version}", fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant) }
            Spacer(Modifier.height(24.dp))
        }
    }
}

/** One group of the table: its label and help line, the basic rows, the deeper levels folded inside it. */
@Composable
private fun GroupCard(
    c: SettingsCard,
    open: Boolean,
    foldOpen: Boolean,
    onToggle: () -> Unit,
    onToggleFold: () -> Unit,
    row: @Composable (ParamSpec) -> Unit,
) {
    BmoeCard(padding = 12.dp) {
        Column(Modifier.fillMaxWidth().clickable(onClick = onToggle)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(c.label, fontWeight = FontWeight.Bold, fontSize = 15.sp, color = MaterialTheme.colorScheme.primary, modifier = Modifier.weight(1f))
                if (c.basic.isEmpty()) Text("${c.moreCount}", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.width(8.dp))
                Text(if (open) "v" else ">", color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (c.help.isNotEmpty()) Hint(c.help)
        }
        if (!open) return@BmoeCard
        c.basic.forEach { row(it) }
        if (c.more.isNotEmpty()) {
            val foldable = c.basic.isNotEmpty()
            if (foldable) {
                Text(
                    "${if (foldOpen) "v" else ">"}  ${c.moreCount} more in ${c.label}: ${c.more.joinToString(", ") { it.level }}",
                    fontSize = 13.sp, color = MaterialTheme.colorScheme.tertiary,
                    modifier = Modifier.fillMaxWidth().clickable(onClick = onToggleFold).padding(vertical = 4.dp),
                )
            }
            if (!foldable || foldOpen) {
                c.more.forEach { lv ->
                    Text(levelName(lv.level), fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.tertiary)
                    LEVEL_HINT[lv.level]?.let { Hint(it) }
                    lv.params.forEach { row(it) }
                }
            }
        }
        if (c.hidden > 0) Hint("${c.hidden} more at a deeper level: widen the filter above to see ${if (c.hidden == 1) "it" else "them"}.")
    }
}

/** The settings that are this app's own, never an engine parameter. */
@Composable
private fun AppSettings(serverUp: Boolean, ctx: ContextWarning?, onRaise: (String) -> Unit) {
    val context = LocalContext.current
    Text("This app", fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary)
    var share by remember { mutableStateOf(AppPrefs.shareOnNetwork(context)) }
    var key by remember { mutableStateOf(AppPrefs.apiKey(context)) }
    SwitchRow(
        "Serve on the network",
        "Other devices on this network can use this phone's engine through its OpenAI-compatible " +
            "API, with the key below. Changing this restarts the engine; a loaded model is loaded again. " +
            "Prompts and answers cross the network in clear.",
        share,
    ) { on ->
        share = on
        AppPrefs.setShareOnNetwork(context, on)
        Engine.restart(context)
    }
    if (share) {
        val clipboard = LocalClipboardManager.current
        // The phone's own address on this network, followed as the network changes: what a
        // client types, not a placeholder to work out.
        var lan by remember { mutableStateOf(LanAddress.current(context)) }
        DisposableEffect(Unit) { onDispose(LanAddress.watch(context) { lan = it }) }
        if (lan.isEmpty()) {
            Text(
                "Not on a Wi-Fi or Ethernet network: no other device can reach the engine until the phone joins one.",
                fontSize = 12.sp, color = MaterialTheme.colorScheme.error,
            )
        } else {
            Text("Address:", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            lan.forEach { ip ->
                val url = LanAddress.url(ip)
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(url, fontFamily = CodeFont, fontSize = 13.sp, modifier = Modifier.weight(1f))
                    TextButton(shape = Corner, onClick = { clipboard.setText(AnnotatedString(url)) }) { Text("Copy") }
                }
            }
        }
        // Where the clients are switched on, what they need: said here whether or not the status
        // line's warning was dismissed.
        if (ctx?.short == true) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(ctx.text(), fontSize = 12.sp, color = warnColor(), modifier = Modifier.weight(1f))
                TextButton(shape = Corner, onClick = { onRaise(ctx.key) }) { Text("Raise ${ctx.label}") }
            }
        }
        Text("Key:", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(key, fontFamily = CodeFont, fontSize = 13.sp, modifier = Modifier.weight(1f))
            TextButton(shape = Corner, onClick = { clipboard.setText(AnnotatedString(key)) }) { Text("Copy") }
            TextButton(shape = Corner, onClick = { key = AppPrefs.regenerateApiKey(context); Engine.restart(context) }) { Text("New key") }
        }
    }
    var csv by remember { mutableStateOf(AppPrefs.metricsCsv(context)) }
    SwitchRow(
        "Metrics CSV",
        "One file per loaded model: per-token timings, faults, cache figures and where memory sat. " +
            "Read and shared from Metrics.",
        csv,
    ) { on -> csv = on; AppPrefs.setMetricsCsv(context, on) }
    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        OutlinedButton(shape = Corner, onClick = { Engine.stop(context) }, enabled = serverUp, modifier = Modifier.weight(1f)) { Text("Stop engine") }
        OutlinedButton(shape = Corner, onClick = { Engine.start(context) }, enabled = !serverUp, modifier = Modifier.weight(1f)) { Text("Start engine") }
    }
}

/**
 * One parameter: its label, the control its type asks for, one chip for where its value came from,
 * and the way back (to the plan's value or the default). A row a link [landing] on scrolls into view,
 * is outlined, and its field takes the focus.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ParamRow(
    spec: ParamSpec,
    cfg: ConfigState,
    decision: PlanDecision?,
    readOnly: Boolean,
    landing: Boolean,
    onSet: (Any?) -> Unit,
    onReset: () -> Unit,
) {
    val value = cfg.value(spec.key)
    val mine = spec.key in cfg.userKeys
    val source = valueSource(mine, decision)
    val planOffer = mine && differsFromPlan(value, decision)
    val bring = remember { BringIntoViewRequester() }
    val focus = remember { FocusRequester() }
    LaunchedEffect(landing) {
        if (!landing) return@LaunchedEffect
        delay(100) // the card and fold this row sits in were opened in the same frame: let them lay out
        bring.bringIntoView()
        if (!readOnly && spec.type != "bool" && spec.type != "choice") runCatching { focus.requestFocus() }
    }
    var mod = Modifier.fillMaxWidth().bringIntoViewRequester(bring)
    if (landing) mod = mod.border(1.dp, tokens.accent, Corner).padding(6.dp)
    Column(mod, verticalArrangement = Arrangement.spacedBy(2.dp)) {
        when {
            readOnly -> {
                Text(spec.label, fontSize = 15.sp)
                Text(paramValueText(value).substringAfterLast('/').ifEmpty { "none" }, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            spec.type == "bool" -> SwitchRow(spec.label, null, value == true) { onSet(it) }
            spec.type == "choice" -> {
                val idx = spec.choices.indexOfFirst { it.first == value?.toString() }.coerceAtLeast(0)
                LabeledDropdown(spec.label, spec.choices.map { it.second }, idx) { i -> onSet(spec.choices[i].first) }
            }
            else -> NumberOrTextField(spec, value, focus, onSet)
        }
        val (lead, rest) = spec.helpParts()
        var more by remember(spec.key) { mutableStateOf(false) }
        if (lead.isNotEmpty()) Hint(lead)
        val extra = listOfNotNull(
            rest.ifEmpty { null },
            spec.special.takeIf { it.isNotEmpty() }?.joinToString("; ") { "${it.first} = ${it.second}" },
        )
        if (extra.isNotEmpty()) {
            if (more) extra.forEach { Hint(it) }
            Text(
                if (more) "Less" else "More",
                fontSize = 12.sp,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.clickable { more = !more }.padding(vertical = 2.dp),
            )
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            if (!readOnly) Chip(source.chip, if (source == ValueSource.DEFAULT) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.primary)
            if (spec.lossy) Chip("changes the reply", warnColor())
            if (spec.scope == "request") Chip("per message", MaterialTheme.colorScheme.onSurfaceVariant)
            if (spec.key in cfg.reloadKeys) Chip("needs reload", warnColor())
            Spacer(Modifier.weight(1f))
            if (!readOnly && mine) {
                val label = when {
                    planOffer -> "Use plan value" + (decision?.valueText?.ifEmpty { decision.value }?.let { ": $it" } ?: "")
                    source == ValueSource.YOU && decision != null && decision.group.ifEmpty { "set" } == "set" -> "Let the plan set it"
                    else -> "Reset"
                }
                TextButton(shape = Corner, onClick = onReset, contentPadding = PaddingValues(horizontal = 8.dp)) { Text(label, fontSize = 12.sp) }
            }
        }
    }
}

/** A number (or `auto`, where the row accepts it) or free text; committed when the field is left. */
@Composable
private fun NumberOrTextField(spec: ParamSpec, value: Any?, focus: FocusRequester, onSet: (Any?) -> Unit) {
    var text by remember(spec.key, value) { mutableStateOf(paramValueText(value)) }
    var error by remember(spec.key) { mutableStateOf<String?>(null) }
    fun commit() {
        if (text == paramValueText(value)) return
        val v = paramValueFromText(spec, text)
        if (v == null) {
            error = "not a ${spec.type}" + if (spec.acceptsAuto) " (or auto)" else ""
            return
        }
        val n = (v as? Number)?.toDouble()
        if (n != null && ((spec.min != null && n < spec.min) || (spec.max != null && n > spec.max))) {
            // The limits appear only here, when a value is refused; the field otherwise shows what is usual.
            error = if (spec.max != null) "between ${paramValueText(spec.min)} and ${paramValueText(spec.max)}"
            else "at least ${paramValueText(spec.min)}"
            return
        }
        error = null
        onSet(v)
    }
    // Below the field: the problem, else what the value means (= off) and what is usual.
    val hint = error ?: listOfNotNull(spec.specialLabel(value)?.let { "= $it" }, spec.typicalText()).joinToString(" · ").ifEmpty { null }
    OutlinedTextField(
        value = text,
        onValueChange = { text = it; error = null },
        label = { Text(spec.label) },
        suffix = spec.unit?.let { { Text(it, fontSize = 12.sp) } },
        isError = error != null,
        supportingText = hint?.let { { Text(it) } },
        singleLine = spec.type != "text",
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
        keyboardActions = KeyboardActions(onDone = { commit() }),
        modifier = Modifier.fillMaxWidth().focusRequester(focus).onFocusChanged { if (!it.isFocused) commit() },
    )
}
