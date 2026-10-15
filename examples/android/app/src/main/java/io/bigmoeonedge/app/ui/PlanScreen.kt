package io.bigmoeonedge.app.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.engine.PlanDecision
import io.bigmoeonedge.app.engine.PlanResult
import kotlinx.coroutines.launch

/**
 * The plan: what the hardware planner measured on this phone and what it decided for the chosen
 * model, with the fact behind each knob. With auto-plan on, every load plans first; here the plan
 * is read, measured again by hand, or applied to the settings.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PlanScreen(modifier: Modifier = Modifier) {
    val engine by Engine.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }
    var plan by remember { mutableStateOf<PlanResult?>(null) }
    var busy by remember { mutableStateOf(false) }
    var showExplain by remember { mutableStateOf(false) }

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
    val applied = engine.config?.plan

    Scaffold(
        modifier = modifier,
        topBar = { TopAppBar(title = { Text("Plan") }, navigationIcon = { Box(Modifier.padding(start = 16.dp)) { BrandMark(wordmark = false) } }) },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        Column(
            Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            val cfg = engine.config
            val planner = engine.info?.plannerAvailable ?: true
            if (!planner) {
                Text("This build does not include the hardware planner.", color = MaterialTheme.colorScheme.error)
            }
            SwitchRow(
                "Plan before every load",
                "The engine measures the phone (storage, memory, each compute device) and chooses the " +
                    "streaming settings for the model, then loads. Values you set yourself are never overridden.",
                checked = cfg?.autoPlan ?: false,
                enabled = planner && cfg != null,
            ) { on -> scope.launch { Engine.setAutoPlan(on) } }

            val model = cfg?.text("model").orEmpty()
            if (model.isEmpty()) {
                Text("Choose a model from Models: a plan is for one model on this phone.", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text("Model: ${model.substringAfterLast('/')}", fontSize = 13.sp)
            }

            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Button(shape = RectangleShape, onClick = { fetch(measure = true) }, enabled = planner && model.isNotEmpty() && !busy && engine.serverUp, modifier = Modifier.weight(1f)) {
                    Text(if (busy) "Measuring…" else "Measure")
                }
                OutlinedButton(shape = RectangleShape, 
                    onClick = {
                        scope.launch {
                            Engine.applyPlan().onSuccess { snackbar.showSnackbar("Plan applied to the settings") }
                                .onFailure { snackbar.showSnackbar(it.message ?: "could not apply") }
                        }
                    },
                    enabled = plan?.available == true && plan?.error.isNullOrEmpty() && !busy,
                    modifier = Modifier.weight(1f),
                ) { Text("Apply") }
            }
            if (busy) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                    Text("Measuring the phone: a few seconds of storage reads and a matmul on each device.", fontSize = 12.sp)
                }
            }

            val p = plan
            when {
                p == null && !applied.isNullOrEmpty() -> {
                    Text("The plan in the settings now:", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    applied.forEach { DecisionRow(it) }
                }
                p == null -> {}
                !p.available -> Text(p.error, color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
                p.error.isNotEmpty() -> Text(p.error, color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
                else -> {
                    ElevatedCard(Modifier.fillMaxWidth()) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text(p.machine, fontSize = 13.sp, fontWeight = FontWeight.Medium)
                            Text(
                                buildString {
                                    append("regime ").append(p.regime)
                                    if (p.declined) append(" · streaming declined: ").append(p.declineReason)
                                    if (p.fromLastLoad) append(" · the plan the loaded model runs with")
                                },
                                fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            if (p.warning.isNotEmpty()) Text(p.warning, fontSize = 12.sp, color = MaterialTheme.colorScheme.error)
                        }
                    }
                    p.decisions.forEach { DecisionRow(it) }
                    if (p.notApplicable.isNotEmpty()) {
                        Text("Decided, but not a setting this app can hold: ${p.notApplicable.joinToString("; ")}", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    if (p.args.isNotEmpty()) {
                        Text("Reproduce with", fontSize = 12.sp, fontWeight = FontWeight.Medium)
                        androidx.compose.foundation.text.selection.SelectionContainer {
                            Text("bmoe-cli " + p.args.joinToString(" "), fontFamily = FontFamily.Monospace, fontSize = 11.sp)
                        }
                    }
                    if (p.explain.isNotEmpty()) {
                        TextButton(shape = RectangleShape, onClick = { showExplain = !showExplain }) { Text(if (showExplain) "Hide the planner's own account" else "The planner's own account") }
                        if (showExplain) {
                            androidx.compose.foundation.text.selection.SelectionContainer {
                                Text(p.explain, fontFamily = FontFamily.Monospace, fontSize = 10.sp)
                            }
                        }
                    }
                }
            }
            Spacer(Modifier.height(24.dp))
        }
    }
}

/** One knob: what it was set to, where that came from, and why. */
@Composable
private fun DecisionRow(d: PlanDecision) {
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(d.knob, fontFamily = FontFamily.Monospace, fontSize = 13.sp, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
            Text(d.value, fontFamily = FontFamily.Monospace, fontSize = 13.sp)
            SourceBadge(d.source)
        }
        Text(d.reason, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
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
