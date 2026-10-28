package io.bigmoeonedge.app.ui

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
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
import io.bigmoeonedge.app.engine.ValueSource
import io.bigmoeonedge.app.engine.differsFromPlan
import io.bigmoeonedge.app.engine.paramValueFromText
import io.bigmoeonedge.app.engine.paramValueText
import io.bigmoeonedge.app.engine.pinnedKeys
import io.bigmoeonedge.app.engine.resettableKeys
import io.bigmoeonedge.app.engine.settingsCards
import io.bigmoeonedge.app.engine.valueSource
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** The open state's keys for the app's own sections: never a group of the table, whose keys are the engine's. */
private const val APP_SECTION = " app"
private const val SHARE_SECTION = " share"

/**
 * Settings, rendered from the engine's parameter table (`GET /api/params`) and nothing else: one
 * card per group in the table's order listing the rows the table offers a form (docs/screens.md,
 * "Settings"). The groups, rows, types, bounds and help all come from the
 * engine, so a knob added there appears here with no app change. Values live in the server's
 * layers; a value set here is the person's and a plan never overrides it. [target] is a parameter a
 * link points at (the Plan screen, the context warning): the page lands on that row and calls
 * [onLanded]. The few settings that are this app's own close the page, and with the metrics switch
 * on, [onOpenMetrics] is one tap from here.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(modifier: Modifier = Modifier, target: String = "", onLanded: () -> Unit = {}, onOpenMetrics: () -> Unit = {}) {
    val context = LocalContext.current
    val engine by Engine.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }
    // Open state per card, by group: every card starts folded, so the page opens as a short list of
    // what can be set and a person opens the one they came for.
    var openCards by remember { mutableStateOf(mapOf<String, Boolean>()) }
    var csv by remember { mutableStateOf(AppPrefs.metricsCsv(context)) }
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

    // A link to one setting lands on it: its card opened (and the row shown even
    // where the table offers it no form); the row itself scrolls into view. Once per link, so editing
    // another field afterwards does not jump back.
    LaunchedEffect(landing, settable, cfg != null) {
        if (landing.isEmpty() || settable == null || cfg == null) return@LaunchedEffect
        val spec = settable.firstOrNull { it.key == landing }
        onLanded()
        jump = ""
        if (spec == null) return@LaunchedEffect
        openCards = openCards + (spec.group to true)
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
            // First, because it is the one setting that reaches past this phone; its header says the state
            // with the card folded.
            val ctx = contextWarning(engine.info?.model.takeIf { engine.serverUp && engine.info?.ready == true }, params)
            ShareSection(openCards[SHARE_SECTION] == true, { openCards = openCards + (SHARE_SECTION to it) }, ctx) { jump = it }
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
                    val decisions = remember(cfg.plan) { cfg.plan.orEmpty().associateBy { it.knob } }
                    val cards = remember(settable, cfg, landedOn) {
                        settingsCards(settable, pinnedKeys(settable, cfg) + setOfNotNull(landedOn.ifEmpty { null }))
                    }
                    val resettable = remember(settable, cfg) { resettableKeys(cfg.userKeys, cfg.launchKeys, settable) }

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
                        val open = openCards[c.group] == true
                        FoldCard(c.label, c.help, open, onToggle = { openCards = openCards + (c.group to !open) }) {
                            c.params.forEach { spec ->
                                ParamRow(
                                    spec, cfg, decisions[spec.key], readOnly = c.readOnly, landing = spec.key == landedOn,
                                    onSet = { set(spec.key, it) }, onReset = { reset(listOf(spec.key)) },
                                )
                            }
                        }
                    }
                }
            }

            HorizontalDivider(color = tokens.line)
            val appOpen = openCards[APP_SECTION] == true
            FoldCard("This app", "How this phone runs the engine and what it records.", appOpen, onToggle = { openCards = openCards + (APP_SECTION to !appOpen) }) {
                AppSettings(engine.serverUp, csv, onCsv = { csv = it })
            }
            // The measurements are a tool for whoever switched the recording on, not a screen of their own.
            if (csv) {
                BmoeCard(Modifier.clickable(onClick = onOpenMetrics), padding = 12.dp) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Metrics", fontWeight = FontWeight.Bold, fontSize = 15.sp, color = MaterialTheme.colorScheme.primary, modifier = Modifier.weight(1f))
                        Text(">", color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    Hint("The live figures of the current generation and the recorded CSV files.")
                }
            }
            engine.info?.let { Text("Engine ${it.version}", fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant) }
            Spacer(Modifier.height(24.dp))
        }
    }
}

/** A box that folds: its title, a [status] beside it and its help line always, its content once opened. */
@Composable
private fun FoldCard(
    title: String,
    help: String,
    open: Boolean,
    onToggle: () -> Unit,
    status: (@Composable () -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    BmoeCard(padding = 12.dp) {
        Column(Modifier.fillMaxWidth().clickable(onClick = onToggle)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(title, fontWeight = FontWeight.Bold, fontSize = 15.sp, color = MaterialTheme.colorScheme.primary, modifier = Modifier.weight(1f))
                status?.let { Spacer(Modifier.width(8.dp)); it() }
                Spacer(Modifier.width(8.dp))
                Text(if (open) "v" else ">", color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (help.isNotEmpty()) Hint(help)
        }
        if (open) content()
    }
}

/**
 * Serve on the network: the switch, and once on, what a client on another device needs (the
 * address, the key, the context a client asks for) and what sharing costs (the traffic is in clear).
 * The header says the state with the card folded, so whether the phone is reachable is never hidden.
 */
@Composable
private fun ShareSection(open: Boolean, onOpen: (Boolean) -> Unit, ctx: ContextWarning?, onRaise: (String) -> Unit) {
    val context = LocalContext.current
    val clipboard = LocalClipboardManager.current
    var share by remember { mutableStateOf(AppPrefs.shareOnNetwork(context)) }
    var key by remember { mutableStateOf(AppPrefs.apiKey(context)) }
    var showKey by remember { mutableStateOf(false) }
    // The phone's own address on this network, followed as the network changes (only while shared):
    // what a client types, not a placeholder to work out.
    var lan by remember { mutableStateOf(emptyList<String>()) }
    if (share) {
        DisposableEffect(Unit) {
            lan = LanAddress.current(context)
            onDispose(LanAddress.watch(context) { lan = it })
        }
    }
    val first = lan.firstOrNull()
    FoldCard(
        "Serve on the network", "", open, onToggle = { onOpen(!open) },
        status = {
            val (text, colour) = when {
                !share -> "Off" to MaterialTheme.colorScheme.onSurfaceVariant
                first == null -> "On · not on a network" to warnColor()
                else -> "On · ${LanAddress.address(first)}" to MaterialTheme.colorScheme.onSurfaceVariant
            }
            Text(text, fontSize = 12.sp, color = colour, maxLines = 1)
        },
    ) {
        SwitchRow(
            "Serve on the network",
            "Other devices on this network use this phone's engine as an OpenAI-compatible server. Changing this restarts the engine.",
            share,
        ) { on ->
            share = on
            AppPrefs.setShareOnNetwork(context, on)
            Engine.restart(context)
        }
        if (!share) return@FoldCard
        Text("Address", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (lan.isEmpty()) {
            Text(
                "Not on a Wi-Fi or Ethernet network: no other device can reach the engine until the phone joins one.",
                fontSize = 12.sp, color = warnColor(),
            )
        }
        lan.forEach { ip ->
            val address = LanAddress.address(ip)
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(address, fontFamily = CodeFont, fontSize = 15.sp, modifier = Modifier.weight(1f))
                TextButton(shape = Corner, onClick = { clipboard.setText(AnnotatedString(address)) }) { Text("Copy") }
            }
        }
        Text("API key", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            // Masked: the screen may be seen or recorded, and the key is what lets a device in.
            Text(if (showKey) key else "•".repeat(key.length), fontFamily = CodeFont, fontSize = 13.sp, maxLines = 1, modifier = Modifier.weight(1f))
            TextButton(shape = Corner, onClick = { showKey = !showKey }) { Text(if (showKey) "Hide" else "Show") }
            TextButton(shape = Corner, onClick = { clipboard.setText(AnnotatedString(key)) }) { Text("Copy") }
            TextButton(shape = Corner, onClick = { key = AppPrefs.regenerateApiKey(context); Engine.restart(context) }) { Text("New key") }
        }
        first?.let { Hint("In a client: base URL ${LanAddress.url(it)}, and the key above.") }
        // Where the clients are switched on, what they need: said here whether or not the status
        // line's warning was dismissed.
        if (ctx?.short == true) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(ctx.text(), fontSize = 12.sp, color = warnColor(), modifier = Modifier.weight(1f))
                TextButton(shape = Corner, onClick = { onRaise(ctx.key) }) { Text("Raise ${ctx.label}") }
            }
        }
        Text("Prompts and answers cross the network unencrypted: use it on a network you trust.", fontSize = 12.sp, color = warnColor())
    }
}

/** The settings that are this app's own, never an engine parameter, besides sharing. */
@Composable
private fun AppSettings(serverUp: Boolean, csv: Boolean, onCsv: (Boolean) -> Unit) {
    val context = LocalContext.current
    SwitchRow(
        "Metrics",
        "Record a per-token CSV for every loaded model and show the measurements here.",
        csv,
    ) { on -> onCsv(on); AppPrefs.setMetricsCsv(context, on) }
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
