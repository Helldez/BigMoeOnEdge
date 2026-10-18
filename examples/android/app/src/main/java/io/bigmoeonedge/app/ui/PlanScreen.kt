package io.bigmoeonedge.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.engine.Engine
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
 * layout for every machine: each block is a list of whatever the plan holds.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PlanScreen(modifier: Modifier = Modifier) {
    val engine by Engine.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }
    var plan by remember { mutableStateOf<PlanResult?>(null) }
    var busy by remember { mutableStateOf(false) }

    fun fetch(measure: Boolean) {
        if (busy) return
        busy = true
        scope.launch {
            Engine.plan(measure).onSuccess { plan = it }.onFailure { snackbar.showSnackbar(it.message ?: "no plan") }
            busy = false
        }
    }

    // On opening, read the plan the loaded model runs with (no measurement); with nothing loaded the
    // decisions the config carries are shown, and measuring is one tap, since on a phone it takes
    // a while and must not start on its own.
    LaunchedEffect(engine.serverUp, engine.info?.model?.path) {
        if (engine.serverUp && engine.info?.model != null) fetch(measure = false)
    }
    // A finished turn is a new measurement of the plan the model runs with: read it again.
    LaunchedEffect(engine.lastDone) {
        if (engine.lastDone != null && plan?.fromLastLoad == true) fetch(measure = false)
    }
    val applied = engine.config?.plan

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
            val cfg = engine.config
            val planner = engine.info?.plannerAvailable ?: true
            if (!planner) {
                Text("This build does not include the hardware planner.", color = MaterialTheme.colorScheme.error)
            }
            val model = cfg?.text("model").orEmpty()
            if (model.isEmpty()) {
                Text("Choose a model from Models: a plan is for one model on this phone.", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text(model.substringAfterLast('/'), fontFamily = FontFamily.Monospace, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }

            if (busy || engine.planProgress != null) ProgressCard(engine.planProgress)

            val p = plan
            when {
                p == null && !applied.isNullOrEmpty() -> {
                    Text("The plan in the settings now", style = MaterialTheme.typography.titleSmall)
                    Decisions(applied)
                }
                p == null -> {}
                !p.available -> Text(p.error, color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
                p.error.isNotEmpty() -> Text(p.error, color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
                else -> PlanBody(p)
            }

            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Button(
                    shape = RectangleShape,
                    onClick = {
                        scope.launch {
                            Engine.applyPlan().onSuccess { snackbar.showSnackbar("Plan applied to the settings") }
                                .onFailure { snackbar.showSnackbar(it.message ?: "could not apply") }
                        }
                    },
                    enabled = plan?.available == true && plan?.error.isNullOrEmpty() && !busy,
                    modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                ) { Text("Apply plan") }
                OutlinedButton(
                    shape = RectangleShape,
                    onClick = { fetch(measure = true) },
                    enabled = planner && model.isNotEmpty() && !busy && engine.serverUp,
                    modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                ) { Text(if (busy) "Measuring…" else "Measure again") }
            }
            SwitchRow(
                "Plan before every load",
                "The engine measures the phone (storage, memory, each compute device) and chooses the " +
                    "streaming settings for the model, then loads. Values you set yourself are never overridden.",
                checked = cfg?.autoPlan ?: false,
                enabled = planner && cfg != null,
            ) { on -> scope.launch { Engine.setAutoPlan(on) } }
            Spacer(Modifier.height(24.dp))
        }
    }
}

@Composable
private fun PlanBody(p: PlanResult) {
    if (p.warning.isNotEmpty()) Text(p.warning, fontSize = 12.sp, color = MaterialTheme.colorScheme.tertiary)
    Tile {
        if (p.fromLastLoad) StatusBadge("The loaded model runs this plan", okColor())
        Text(p.sentence.ifEmpty { "Regime: ${p.regime}" }, fontFamily = PixelFont, fontSize = 22.sp, lineHeight = 26.sp)
        if (p.machine.isNotEmpty()) Text(p.machine, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            SpeedWell(p, Modifier.weight(1f))
            HitsWell(p, Modifier.weight(1f))
        }
        p.lanes.forEach { lane ->
            Row(
                Modifier.fillMaxWidth().border(2.dp, MaterialTheme.colorScheme.outline).padding(horizontal = 10.dp, vertical = 6.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                Text(lane.label, fontSize = 14.sp)
                Text(lane.on.joinToString(" + "), fontFamily = PixelFont, color = MaterialTheme.colorScheme.primary)
            }
        }
        if (p.notApplicable.isNotEmpty()) {
            Text("Decided, but not a setting this app can hold: ${p.notApplicable.joinToString("; ")}", fontSize = 12.sp, color = MaterialTheme.colorScheme.tertiary)
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
    Decisions(p.decisions)

    if (p.args.isNotEmpty()) Folded("Command") {
        SelectionContainer { Text("bmoe-cli " + p.args.joinToString(" "), fontFamily = FontFamily.Monospace, fontSize = 11.sp) }
    }
    if (p.explain.isNotEmpty()) Folded("The planner's own account") {
        SelectionContainer { Text(p.explain, fontFamily = FontFamily.Monospace, fontSize = 10.sp) }
    }
}

@Composable
private fun SpeedWell(p: PlanResult, modifier: Modifier) {
    val measured = p.measured?.sPerToken?.takeIf { it > 0 }
    val predicted = p.predictedSPerToken.takeIf { it > 0 }
    Well(modifier) {
        Text("Tokens", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        when {
            measured != null -> {
                Figure(fmt(1 / measured), "tok/s measured")
                if (predicted != null) {
                    val delta = 100 * (predicted / measured - 1)
                    Text(
                        String.format(Locale.US, "%+.0f%% against %s", delta, fmt(1 / predicted)),
                        fontSize = 12.sp, color = if (delta >= 0) okColor() else MaterialTheme.colorScheme.tertiary,
                    )
                }
            }
            predicted != null -> {
                Figure(fmt(1 / predicted), "tok/s expected")
                Text("Measured after the first answer", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            else -> Text("No prediction", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun HitsWell(p: PlanResult, modifier: Modifier) {
    val measured = p.measured?.cacheHitPct?.takeIf { it >= 0 }
    val credited = p.predictedHitPct.takeIf { it >= 0 }
    if (measured == null && credited == null) return
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
    val colours = listOf(MaterialTheme.colorScheme.secondary, MaterialTheme.colorScheme.primary, MaterialTheme.colorScheme.onSurfaceVariant, okColor(), MaterialTheme.colorScheme.tertiary)
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(pool.label + if (pool.estimated) " (an even share of the devices' sum)" else "", fontSize = 14.sp)
            if (pool.capBytes > 0) Text("of " + bytesLabel(pool.capBytes), fontFamily = PixelFont, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Row(Modifier.fillMaxWidth().height(28.dp).border(2.dp, MaterialTheme.colorScheme.outline).background(MaterialTheme.colorScheme.surfaceContainerHigh)) {
            held.forEachIndexed { i, r ->
                val w = r.bytes.toFloat() / whole
                if (w > 0f) Box(Modifier.fillMaxHeight().weight(w).background(colours[i % colours.size]))
            }
            if (free > 0) Spacer(Modifier.fillMaxHeight().weight(free.toFloat() / whole))
        }
        held.forEachIndexed { i, r ->
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Box(Modifier.size(12.dp).background(colours[i % colours.size]))
                Text(r.label, fontSize = 14.sp, modifier = Modifier.weight(1f))
                if (r.locked) StatusBadge("locked", MaterialTheme.colorScheme.onSurfaceVariant)
                Text(bytesLabel(r.bytes), fontFamily = PixelFont, fontSize = 14.sp)
            }
        }
        if (free > 0) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Box(Modifier.size(12.dp).border(2.dp, MaterialTheme.colorScheme.outline))
                Text("Free", fontSize = 14.sp, modifier = Modifier.weight(1f))
                Text(bytesLabel(free), fontFamily = PixelFont, fontSize = 14.sp)
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
                Text(d.name, fontFamily = PixelFont, fontSize = 16.sp)
                if (d.description.isNotEmpty() && d.description != d.name) Text(d.description, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            StatusBadge(d.badge, colour)
        }
        val used = d.status == "used"
        Meter(if (d.promptRatio > 0) maxOf(0.02f, (d.promptRatio / scale).toFloat()) else 0f, if (used) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
        if (d.rateText.isNotEmpty()) Text(d.rateText, fontFamily = PixelFont, fontSize = 12.sp)
        Text(d.why, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/** The three groups: the settings the plan decided, open; the ones it left and the notes, folded. */
@Composable
private fun Decisions(all: List<PlanDecision>) {
    val by = all.groupBy { it.readGroup }
    by["set"]?.let { rows -> Tile { Text("Set by the plan · ${rows.size}", fontFamily = PixelFont, fontSize = 16.sp); rows.forEach { DecisionRow(it) } } }
    by["kept"]?.let { rows -> Folded("Left as is · ${rows.size}") { rows.forEach { DecisionRow(it) } } }
    by["notes"]?.let { rows -> Folded("Notes · ${rows.size}") { rows.forEach { DecisionRow(it) } } }
}

/** One knob: its settings label, its value, where that came from; the reason on a tap. */
@Composable
private fun DecisionRow(d: PlanDecision) {
    var open by remember { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth().clickable { open = !open }.padding(vertical = 6.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(if (open) "v" else ">", fontFamily = PixelFont, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(d.label.ifEmpty { d.knob }, fontSize = 14.sp, modifier = Modifier.weight(1f))
            if (d.value.isNotEmpty() && d.value != "-") Text(d.value, fontFamily = PixelFont, fontSize = 14.sp)
            SourceBadge(d.source)
        }
        if (open) Text(d.reason, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(start = 20.dp))
    }
}

@Composable
private fun ProgressCard(p: PlanProgress?) {
    var done by remember { mutableStateOf(listOf<String>()) }
    var last by remember { mutableStateOf("") }
    LaunchedEffect(p) {
        if (p == null) return@LaunchedEffect
        if (p.index == 0) done = emptyList()
        if (last.isNotEmpty() && last != p.label && last !in done) done = done + last
        last = p.label
    }
    Tile {
        Text("Measuring this phone", fontFamily = PixelFont, fontSize = 18.sp)
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

// ── small pieces, in the pixel language: square, 2 dp edges, no rounding ──

@Composable
private fun Tile(content: @Composable ColumnScope.() -> Unit) {
    Column(
        Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.surfaceContainer).border(2.dp, MaterialTheme.colorScheme.outline).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
        content = content,
    )
}

@Composable
private fun Well(modifier: Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier.background(MaterialTheme.colorScheme.background).border(2.dp, MaterialTheme.colorScheme.outline).padding(10.dp),
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
private fun Folded(title: String, content: @Composable ColumnScope.() -> Unit) {
    var open by remember { mutableStateOf(false) }
    Tile {
        Row(Modifier.fillMaxWidth().clickable { open = !open }, verticalAlignment = Alignment.CenterVertically) {
            Text(if (open) "v" else ">", fontFamily = PixelFont, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.width(10.dp))
            Text(title, fontFamily = PixelFont, fontSize = 16.sp)
        }
        if (open) content()
    }
}

@Composable
private fun Figure(value: String, unit: String) {
    Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(value, fontFamily = PixelFont, fontSize = 26.sp, lineHeight = 28.sp)
        Text(unit, fontFamily = PixelFont, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun Meter(fraction: Float, colour: Color, mark: Float? = null) {
    Box(Modifier.fillMaxWidth().height(10.dp).border(2.dp, MaterialTheme.colorScheme.outline).background(MaterialTheme.colorScheme.surfaceContainerHigh)) {
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
    Surface(color = colour.copy(alpha = 0.18f), shape = RectangleShape, modifier = Modifier.border(2.dp, colour)) {
        Text(text, fontFamily = PixelFont, fontSize = 12.sp, color = colour, modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp))
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

@Composable
private fun okColor(): Color = if (isSystemInDarkTheme()) Palette.Ok else Palette.LightOk

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
