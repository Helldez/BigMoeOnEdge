package io.bigmoeonedge.app.ui

import androidx.compose.ui.text.font.FontWeight

import androidx.compose.ui.draw.clip
import androidx.compose.foundation.shape.CircleShape

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.engine.AppliedChange
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.engine.PlanFigures
import io.bigmoeonedge.app.engine.appliedChanges
import io.bigmoeonedge.app.engine.diagnosticText
import io.bigmoeonedge.app.engine.loadTargetChosen
import io.bigmoeonedge.app.engine.planFigures
import io.bigmoeonedge.app.engine.PlanDecision
import io.bigmoeonedge.app.engine.PlanDevice
import io.bigmoeonedge.app.engine.PlanPool
import io.bigmoeonedge.app.engine.PlanProgress
import io.bigmoeonedge.app.engine.PlanResult
import kotlinx.coroutines.launch
import java.util.Locale

/**
 * The plan: what the hardware planner measured on this phone and what it decided for the chosen
 * model, read top down as the web UI reads it (docs/screens.md): one sentence and how it turned
 * out, where the memory goes, what each device does, the decisions grouped, the command. One
 * layout for every machine and the same one on every front-end: each block is a list of whatever
 * the plan holds. The plan itself lives in the engine state, which reads it again when a session
 * opens and when a turn ends, so this screen only draws it.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PlanScreen(modifier: Modifier = Modifier, onOpenSetting: (key: String) -> Unit = {}) {
    val engine by Engine.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }
    var applied by remember { mutableStateOf<List<AppliedChange>?>(null) }

    // The engine reads the plan when it reaches the server, when a session opens and when a turn
    // ends: never a measurement, which on a phone takes a while and must not start on its own. A
    // screen opened before the engine got there asks once.
    LaunchedEffect(engine.serverUp) {
        if (engine.serverUp && engine.plan == null) Engine.refreshPlan()
    }
    fun measure() {
        applied = null
        Engine.measurePlan()
    }

    val cfg = engine.config
    val plan = engine.plan
    val busy = engine.measuring
    val planner = engine.info?.plannerAvailable ?: true
    // A plan is for the model a load opens: chosen on Models, which the table marks as a read-only group.
    val chosen = engine.info?.model != null || loadTargetChosen(engine.params, cfg)
    val canMeasure = planner && chosen && !busy && engine.serverUp
    // One banner for whatever went wrong: the request, or a plan the server could not make.
    val shownError = engine.planError.ifEmpty { plan?.takeIf { it.available }?.error.orEmpty() }

    Scaffold(
        modifier = modifier,
        topBar = {
            TopAppBar(
                title = { Text("Plan") },
                navigationIcon = { Box(Modifier.padding(start = 16.dp)) { BrandMark(wordmark = false) } },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        Column(
            Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            if (!planner) Text("This build does not include the hardware planner.", color = MaterialTheme.colorScheme.error)
            if (!chosen && engine.params != null) {
                Text("Choose a model from Models: a plan is for one model on this phone.", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            // A failed plan can be retried where it says so: measuring is the person's tap, never automatic.
            if (shownError.isNotEmpty()) RetryLine(shownError, canMeasure) { measure() }
            if (busy || engine.planProgress != null) PlanProgressCard(engine.planProgress)

            val p = plan
            if (p?.available == true && cfg != null) {
                AutoPlanSwitch(cfg.autoPlan, planner) { on -> scope.launch { Engine.setAutoPlan(on) } }
            }
            when {
                p == null -> {}
                !p.available -> Text(p.error, color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
                p.error.isNotEmpty() -> {} // in the banner above
                p.none -> Tile {
                    Text("Not measured yet.", fontSize = 22.sp, lineHeight = 26.sp)
                    Text("Measure this phone to see what the plan does with the chosen model.", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    PrimaryAction(if (busy) "Measuring..." else "Measure", enabled = canMeasure) { measure() }
                }
                else -> PlanBody(
                    p,
                    canApply = !busy,
                    onApply = {
                        val before = engine.config?.values.orEmpty()
                        applied = null
                        scope.launch {
                            Engine.applyPlan().onSuccess { c -> applied = appliedChanges(before, c.values, engine.params.orEmpty()) }
                                .onFailure { snackbar.showSnackbar(it.message ?: "could not apply") }
                        }
                    },
                    applied = applied,
                    canMeasure = canMeasure,
                    measuring = busy,
                    onMeasure = { measure() },
                    reloadNeeded = engine.ready && cfg?.reloadRequired == true,
                    onOpenSetting = onOpenSetting,
                    onCopied = { scope.launch { snackbar.showSnackbar("Copied: the command and the planner's account") } },
                )
            }
            // With nothing loaded and no plan to show, the decisions the configuration carries from the
            // last applied plan, grouped the same way: the screen is never empty once a plan was made.
            val carried = cfg?.plan.orEmpty()
            if (engine.info?.model == null && carried.isNotEmpty() && (p == null || p.none || p.error.isNotEmpty() || !p.available)) {
                Text("Decisions from the last applied plan", style = MaterialTheme.typography.titleMedium)
                Hint("What the settings carry now; Measure shows the plan in full.")
                Decisions(carried, onOpenSetting)
            }
            Spacer(Modifier.height(24.dp))
        }
    }
}

@Composable
private fun PrimaryAction(text: String, enabled: Boolean, onClick: () -> Unit) {
    Button(shape = Corner, onClick = onClick, enabled = enabled, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp)) {
        Text(text, fontSize = 16.sp)
    }
}

@Composable
private fun AutoPlanSwitch(checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().heightIn(min = 44.dp), verticalAlignment = Alignment.CenterVertically) {
        Text("Plan before every load", fontSize = 14.sp, modifier = Modifier.weight(1f))
        Switch(checked = checked, onCheckedChange = onChange, enabled = enabled)
    }
}

/** An error with the way to try again: Measure again, beside it. */
@Composable
private fun RetryLine(error: String, canMeasure: Boolean, onMeasure: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(error, color = MaterialTheme.colorScheme.error, fontSize = 13.sp, modifier = Modifier.weight(1f))
        OutlinedButton(shape = Corner, onClick = onMeasure, enabled = canMeasure) { Text("Measure again") }
    }
}

@Composable
private fun PlanBody(
    p: PlanResult,
    canApply: Boolean,
    onApply: () -> Unit,
    applied: List<AppliedChange>?,
    canMeasure: Boolean,
    measuring: Boolean,
    onMeasure: () -> Unit,
    reloadNeeded: Boolean,
    onOpenSetting: (String) -> Unit,
    onCopied: () -> Unit,
) {
    if (p.warning.isNotEmpty()) Text(p.warning, fontSize = 12.sp, color = MaterialTheme.colorScheme.tertiary)
    val f = planFigures(p)
    Tile {
        when {
            p.fromLastLoad -> StatusBadge("The loaded model runs this plan", okColor())
            p.cached -> StatusBadge("The last plan measured", MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Text(p.sentence.ifEmpty { "Regime: ${p.regime}" }, fontSize = 22.sp, lineHeight = 26.sp)
        if (p.machine.isNotEmpty()) Text(p.machine, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (p.lanes.isNotEmpty()) {
            Text(
                p.lanes.joinToString("   ") { "${it.label} ${it.on.joinToString(" + ")}" },
                fontSize = 13.sp, color = MaterialTheme.colorScheme.primary,
            )
        }
        // The three figures: decode, cache hits, memory; two to a row on a phone.
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            SpeedWell(f, p.fromLastLoad, Modifier.weight(1f))
            HitsWell(f, Modifier.weight(1f))
        }
        if (f.capBytes != null) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                MemoryWell(f, Modifier.weight(1f))
                Spacer(Modifier.weight(1f))
            }
        }
        if (p.notApplicable.isNotEmpty()) {
            Text("Decided, but not a setting this app can hold: ${p.notApplicable.joinToString("; ")}", fontSize = 12.sp, color = MaterialTheme.colorScheme.tertiary)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(shape = Corner, onClick = onApply, enabled = canApply, modifier = Modifier.weight(1f).heightIn(min = 48.dp)) { Text("Apply plan") }
            OutlinedButton(shape = Corner, onClick = onMeasure, enabled = canMeasure, modifier = Modifier.weight(1f).heightIn(min = 48.dp)) {
                Text(if (measuring) "Measuring..." else "Measure again")
            }
        }
        Text("Values you set yourself are never changed.", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        applied?.let { changes ->
            if (changes.isEmpty()) {
                Text("Plan applied: every setting already had the plan's value.", fontSize = 13.sp, color = okColor())
            } else {
                Text("Plan applied. ${if (changes.size == 1) "One setting changed" else "${changes.size} settings changed"}:", fontSize = 13.sp, color = okColor())
                changes.forEach { c ->
                    Row(Modifier.fillMaxWidth().clickable { onOpenSetting(c.key) }.padding(vertical = 2.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text(c.label, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary, modifier = Modifier.weight(1f))
                        Text("${c.from} → ${c.to}", style = FigureStyle, fontSize = 13.sp)
                    }
                }
            }
            if (reloadNeeded) Text("Some applied values need a model reload: Settings has the Reload.", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }

    if (p.pools.isNotEmpty()) {
        Section("Memory") { p.pools.forEach { MemoryPool(it) } }
    }
    if (p.devices.isNotEmpty()) {
        Section("Devices") {
            val scale = p.devices.maxOf { it.promptRatio }.coerceAtLeast(1.0)
            p.devices.forEach { DeviceRow(it, scale) }
            if (p.devicesNoneText.isNotEmpty()) Text(p.devicesNoneText, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
    Text("Decisions", style = MaterialTheme.typography.titleMedium)
    Decisions(p.decisions, onOpenSetting)

    if (p.args.isNotEmpty()) Folded("Command") {
        SelectionContainer { Text("bmoe-cli " + p.args.joinToString(" "), fontFamily = CodeFont, fontSize = 11.sp) }
    }
    // The planner's whole account is not shown (every line of it has a worded twin above); it goes on
    // the clipboard with the command, for a bug report.
    val clipboard = LocalClipboardManager.current
    val uri = LocalUriHandler.current
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        OutlinedButton(shape = Corner, onClick = { clipboard.setText(AnnotatedString(diagnosticText(p.args, p.explain))); onCopied() }, modifier = Modifier.weight(1f)) {
            Text("Copy diagnostic")
        }
        TextButton(shape = Corner, onClick = { runCatching { uri.openUri(PLANNING_DOC) } }, modifier = Modifier.weight(1f)) { Text("How the planner decides") }
    }
}

private const val PLANNING_DOC = "https://github.com/Helldez/BigMoeOnEdge/blob/main/docs/hardware-planning.md"

@Composable
private fun SpeedWell(f: PlanFigures, fromLastLoad: Boolean, modifier: Modifier) {
    Well(modifier) {
        Text("Tokens", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        when {
            f.measuredTokS != null -> {
                Figure(fmt(f.measuredTokS), "tok/s measured")
                if (f.deltaPct != null && f.predictedTokS != null) {
                    if (fromLastLoad) {
                        Text(
                            String.format(Locale.US, "%+.0f%% against the %s expected", f.deltaPct, fmt(f.predictedTokS)),
                            fontSize = 12.sp, color = if (f.deltaPct >= 0) okColor() else warnColor(),
                        )
                    } else {
                        Text("${fmt(f.predictedTokS)} tok/s expected under this plan", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
            f.predictedTokS != null -> {
                Figure(fmt(f.predictedTokS), "tok/s expected")
                Text(if (fromLastLoad) "Measured after the first answer" else "Measured after a load", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            else -> Text("No prediction", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun MemoryWell(f: PlanFigures, modifier: Modifier) {
    val cap = f.capBytes
    if (cap == null) {
        Spacer(modifier)
        return
    }
    val held = f.heldBytes ?: 0
    Well(modifier) {
        Text("Memory", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Figure(bytesLabel(held), "held")
        Meter((held.toDouble() / cap).toFloat(), MaterialTheme.colorScheme.primary)
        Text("Of the ${bytesLabel(cap)} it may hold", fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun HitsWell(f: PlanFigures, modifier: Modifier) {
    val measured = f.measuredHitPct
    val credited = f.creditedHitPct
    if (measured == null && credited == null) {
        Spacer(modifier)
        return
    }
    Well(modifier) {
        Text("Cache hits", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Figure(String.format(Locale.US, "%.0f%%", measured ?: credited), if (measured != null) "measured" else "credited")
        Meter(((measured ?: credited ?: 0.0) / 100).toFloat(), MaterialTheme.colorScheme.primary, mark = if (measured != null) credited?.let { (it / 100).toFloat() } else null)
        if (measured != null && credited != null) {
            Text(String.format(Locale.US, "The mark is the %.0f%% credited", credited), fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/** One pool as one bar: every held row, then what is free, against the larger of the cap and the sum. */
@Composable
private fun MemoryPool(pool: PlanPool) {
    val held = pool.rows.filter { !it.inTurn }
    val sum = held.sumOf { it.bytes }
    val whole = maxOf(pool.capBytes, sum).coerceAtLeast(1)
    val free = whole - sum
    // The categorical series, not the theme's roles: those look alike side by side, and a bar of
    // processes is read by telling its segments apart.
    val colours = tokens.series
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(pool.label + if (pool.estimated) " (an even share of the devices' sum)" else "", fontSize = 14.sp)
            if (pool.capBytes > 0) Text("of " + bytesLabel(pool.capBytes), fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Row(Modifier.fillMaxWidth().height(24.dp).clip(Corner).background(MaterialTheme.colorScheme.surfaceContainerHigh)) {
            held.forEachIndexed { i, r ->
                val w = r.bytes.toFloat() / whole
                if (w > 0f) Box(Modifier.fillMaxHeight().weight(w).background(colours[i % colours.size]))
            }
            if (free > 0) Spacer(Modifier.fillMaxHeight().weight(free.toFloat() / whole))
        }
        held.forEachIndexed { i, r ->
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Box(Modifier.size(12.dp).clip(MaterialTheme.shapes.extraSmall).background(colours[i % colours.size]))
                Text(r.label, fontSize = 14.sp, modifier = Modifier.weight(1f))
                if (r.locked) StatusBadge("locked", MaterialTheme.colorScheme.onSurfaceVariant)
                Text(bytesLabel(r.bytes), fontSize = 14.sp)
            }
        }
        if (free > 0) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Box(Modifier.size(12.dp).border(1.dp, MaterialTheme.colorScheme.outline, MaterialTheme.shapes.extraSmall))
                Text("Free", fontSize = 14.sp, modifier = Modifier.weight(1f))
                Text(bytesLabel(free), fontSize = 14.sp)
            }
        }
        pool.rows.filter { it.inTurn }.forEach {
            Text("${it.label}: ${bytesLabel(it.bytes)} borrowed from the expert cache while a prompt runs, given back after.", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun DeviceRow(d: PlanDevice, scale: Double) {
    val colour = deviceColour(d)
    Column(Modifier.fillMaxWidth().padding(vertical = 6.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Column(Modifier.weight(1f)) {
                Text(d.name, fontSize = 16.sp)
                if (d.description.isNotEmpty() && d.description != d.name) Text(d.description, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            StatusBadge(d.badge, colour)
        }
        val used = d.status == "used"
        Meter(if (d.promptRatio > 0) maxOf(0.02f, (d.promptRatio / scale).toFloat()) else 0f, if (used) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
        if (d.rateText.isNotEmpty()) Text(d.rateText, fontSize = 12.sp)
        Text(d.why, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/** The three groups: the settings the plan decided, open; the ones it left and the notes, folded. */
@Composable
private fun Decisions(all: List<PlanDecision>, onOpenSetting: (String) -> Unit) {
    val by = all.groupBy { it.group.ifEmpty { "set" } }
    by["set"]?.let { rows -> Tile { Text("Set by the plan · ${rows.size}", fontSize = 16.sp); rows.forEach { DecisionRow(it, onOpenSetting) } } }
    by["kept"]?.let { rows ->
        Folded("Left as is · ${rows.size}", "Settings the plan looked at and left as they were.") { rows.forEach { DecisionRow(it, onOpenSetting) } }
    }
    by["notes"]?.let { rows ->
        Folded("Notes · ${rows.size}", "What the plan measured and weighed. None of these is a setting.") { rows.forEach { DecisionRow(it, onOpenSetting) } }
    }
}

/** One decision: its label and value; on a tap the one-line brief, the full reason behind Why, and the setting. */
@Composable
private fun DecisionRow(d: PlanDecision, onOpenSetting: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    var why by remember { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth().clickable { open = !open }.padding(vertical = 6.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(if (open) "v" else ">", color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(d.label.ifEmpty { d.knob }, fontSize = 14.sp, modifier = Modifier.weight(1f))
            val shown = d.valueText.ifEmpty { d.value.takeIf { it != "-" }.orEmpty() }
            if (shown.isNotEmpty()) Text(shown, fontSize = 14.sp)
            SourceBadge(d.source)
        }
        if (open) {
            Column(Modifier.padding(start = 20.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(d.brief.ifEmpty { d.reason }, fontSize = 13.sp)
                if (d.brief.isNotEmpty() && d.brief != d.reason) {
                    Text(if (why) "Why  v" else "Why  >", fontSize = 13.sp, color = MaterialTheme.colorScheme.primary, modifier = Modifier.clickable { why = !why })
                    if (why) Text(d.reason, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                if (d.label.isNotEmpty() && d.group != "notes") {
                    Text("${d.label} in Settings", fontSize = 13.sp, color = MaterialTheme.colorScheme.primary, modifier = Modifier.clickable { onOpenSetting(d.knob) })
                }
            }
        }
    }
}

/** A measurement in progress, in the server's words: done, running, how many to go. Models shows it during a load too. */
@Composable
fun PlanProgressCard(p: PlanProgress?) {
    var done by remember { mutableStateOf(listOf<String>()) }
    var last by remember { mutableStateOf("") }
    LaunchedEffect(p) {
        if (p == null) return@LaunchedEffect
        if (p.index == 0) done = emptyList()
        if (last.isNotEmpty() && last != p.label && last !in done) done = done + last
        last = p.label
    }
    Tile {
        Text("Measuring this phone", fontSize = 18.sp)
        Meter(if (p != null && p.count > 0) p.index.toFloat() / p.count else 0f, MaterialTheme.colorScheme.primary)
        done.forEach { StepLine("done", okColor(), it) }
        if (p != null) {
            StepLine("running", MaterialTheme.colorScheme.primary, p.label)
            val left = p.count - p.index - 1
            if (left > 0) StepLine("next", MaterialTheme.colorScheme.onSurfaceVariant, "$left more")
        }
    }
}

@Composable
private fun StepLine(state: String, colour: Color, label: String) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Box(Modifier.widthIn(min = 72.dp)) { StatusBadge(state, colour) }
        Text(label, fontSize = 14.sp)
    }
}

// ── small pieces: the card, the well, the meter, the badge (docs/design.md) ──

@Composable
private fun Tile(content: @Composable ColumnScope.() -> Unit) {
    BmoeCard(content = content)
}

@Composable
private fun Well(modifier: Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier.background(tokens.surface2, Corner).padding(10.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
        content = content,
    )
}

@Composable
private fun Section(title: String, content: @Composable ColumnScope.() -> Unit) {
    Text(title, style = MaterialTheme.typography.titleMedium)
    Tile(content)
}

@Composable
private fun Folded(title: String, hint: String = "", content: @Composable ColumnScope.() -> Unit) {
    var open by remember { mutableStateOf(false) }
    Tile {
        Row(Modifier.fillMaxWidth().clickable { open = !open }, verticalAlignment = Alignment.CenterVertically) {
            Text(if (open) "v" else ">", color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.width(10.dp))
            Text(title, fontSize = 16.sp)
        }
        if (hint.isNotEmpty()) Text(hint, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (open) content()
    }
}

@Composable
private fun Figure(value: String, unit: String) {
    Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(value, fontSize = 26.sp, lineHeight = 28.sp)
        Text(unit, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun Meter(fraction: Float, colour: Color, mark: Float? = null) {
    Box(Modifier.fillMaxWidth().height(8.dp).clip(CircleShape).background(MaterialTheme.colorScheme.surfaceContainerHigh)) {
        Box(Modifier.fillMaxHeight().fillMaxWidth(fraction.coerceIn(0f, 1f)).background(colour))
        mark?.let { m ->
            Row(Modifier.fillMaxSize()) {
                if (m > 0f) Spacer(Modifier.fillMaxHeight().weight(m.coerceIn(0.001f, 1f)))
                Box(Modifier.fillMaxHeight().width(2.dp).background(MaterialTheme.colorScheme.onSurface))
                if (m < 1f) Spacer(Modifier.fillMaxHeight().weight((1f - m).coerceIn(0.001f, 1f)))
            }
        }
    }
}

@Composable
private fun StatusBadge(text: String, colour: Color) {
    Surface(color = tintFor(colour), shape = MaterialTheme.shapes.extraSmall) {
        Text(text, fontSize = 12.sp, fontWeight = FontWeight.Medium, color = colour, modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp))
    }
}

/** measured | derived | policy | operator | unprobed, as a small label: where a value came from. */
@Composable
fun SourceBadge(source: String) {
    val color = when (source) {
        "measured" -> MaterialTheme.colorScheme.primary
        "operator" -> MaterialTheme.colorScheme.tertiary
        "unprobed" -> MaterialTheme.colorScheme.error
        else -> MaterialTheme.colorScheme.onSurfaceVariant
    }
    Surface(color = color.copy(alpha = 0.12f), shape = MaterialTheme.shapes.extraSmall) {
        Text(source, fontSize = 10.sp, color = color, modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp))
    }
}


/** A badge's colour, from a device's role alone; its words come from the server. */
@Composable
private fun deviceColour(d: PlanDevice): Color = when (d.status) {
    "used" -> okColor()
    "refused", "unusable" -> MaterialTheme.colorScheme.error
    else -> MaterialTheme.colorScheme.onSurfaceVariant
}

private fun fmt(v: Double) = String.format(Locale.US, "%.1f", v)

/** Binary units, as the web UI shows them, so a figure reads the same on both. */
private fun bytesLabel(bytes: Long): String {
    val units = listOf("B", "KiB", "MiB", "GiB", "TiB")
    var v = bytes.toDouble()
    var u = 0
    while (v >= 1024 && u < units.size - 1) {
        v /= 1024
        u++
    }
    return String.format(Locale.US, if (u == 0 || v >= 100) "%.0f %s" else "%.1f %s", v, units[u])
}
