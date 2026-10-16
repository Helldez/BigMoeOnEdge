package io.bigmoeonedge.app.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.RectangleShape
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
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.engine.ParamSpec
import io.bigmoeonedge.app.engine.ServerProcess
import io.bigmoeonedge.app.engine.paramValueFromText
import io.bigmoeonedge.app.engine.paramValueText
import kotlinx.coroutines.launch

/**
 * Settings, rendered from the engine's parameter table (`GET /api/params`) and nothing else: the
 * groups, the rows, their types, bounds and help all come from the engine, so a knob added there
 * appears here with no app change. Values live in the server's layers; a value set here is the
 * person's and a plan never overrides it. The few settings that are this app's own close the page.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val engine by Engine.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }
    var advancedOpen by rememberSaveable { mutableStateOf(false) }

    fun set(key: String, value: Any?) = scope.launch {
        Engine.setConfig(mapOf(key to value)).onFailure { snackbar.showSnackbar(it.message ?: "not saved") }
    }
    fun reset(key: String) = scope.launch {
        Engine.setConfig(emptyMap(), listOf(key)).onFailure { snackbar.showSnackbar(it.message ?: "not reset") }
    }

    Scaffold(
        modifier = modifier,
        topBar = { TopAppBar(title = { Text("Settings") }, navigationIcon = { Box(Modifier.padding(start = 16.dp)) { BrandMark(wordmark = false) } }) },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        val params = engine.params
        val cfg = engine.config
        Column(
            Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            if (cfg?.reloadRequired == true) {
                ElevatedCard(Modifier.fillMaxWidth()) {
                    Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text("A setting changed that is fixed while a model is loaded. Reload to apply it.", fontSize = 13.sp, modifier = Modifier.weight(1f))
                        TextButton(shape = RectangleShape, onClick = { scope.launch { Engine.load(null).onFailure { snackbar.showSnackbar(it.message ?: "reload failed") } } }) { Text("Reload") }
                    }
                }
            }
            if (cfg != null && !cfg.valid) {
                Text("The configuration does not validate: ${cfg.error}", color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
            }

            when {
                !engine.serverUp -> Text("The engine is not running.", color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
                params == null || cfg == null -> Text("Waiting for the engine…", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                else -> {
                    // A path other than the model is set at launch, not over the API: not a setting here.
                    val settable = params.filter { it.type != "path" || it.key == "model" }
                    val groups = ParamSpec.grouped(settable)
                    // The recommended configuration first: each group's basic rows, in the table's order.
                    groups.forEach { (groupLabel, rows) ->
                        val basic = rows.filter { it.level == "basic" }
                        if (basic.isEmpty()) return@forEach
                        Text(groupLabel, fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary)
                        basic.forEach { spec ->
                            ParamRow(spec, cfg, modelName = engine.info?.model?.name, onSet = { set(spec.key, it) }, onReset = { reset(spec.key) })
                        }
                    }
                    // Every row in one section of its own, folded, the basic ones included: the whole
                    // engine in one place, each row marked with its level. They ship on purpose, because
                    // measuring them on other hardware is part of what this app is for; a lever nobody
                    // can reach is a lever nobody can refute.
                    AdvancedSection(advancedOpen, onToggle = { advancedOpen = !advancedOpen }) {
                        groups.forEach { (groupLabel, rows) ->
                            Text(groupLabel, fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.tertiary)
                            rows.forEach { spec ->
                                ParamRow(spec, cfg, modelName = engine.info?.model?.name, onSet = { set(spec.key, it) }, onReset = { reset(spec.key) })
                            }
                        }
                    }
                }
            }

            HorizontalDivider()
            Text("This app", fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary)
            var share by remember { mutableStateOf(AppPrefs.shareOnNetwork(context)) }
            var key by remember { mutableStateOf(AppPrefs.apiKey(context)) }
            SwitchRow(
                "Serve on the network",
                "Other devices on this network can use this phone's engine through its OpenAI-compatible " +
                    "API, with the key below. Changing this restarts the engine, which unloads the model. " +
                    "Prompts and answers cross the network in clear.",
                share,
            ) { on ->
                share = on
                AppPrefs.setShareOnNetwork(context, on)
                Engine.restart(context)
            }
            if (share) {
                val clipboard = LocalClipboardManager.current
                Text("Address: http://<this phone's IP>:${ServerProcess.PORT}/v1 · key:", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(key, fontFamily = FontFamily.Monospace, fontSize = 13.sp, modifier = Modifier.weight(1f))
                    TextButton(shape = RectangleShape, onClick = { clipboard.setText(AnnotatedString(key)) }) { Text("Copy") }
                    TextButton(shape = RectangleShape, onClick = { key = AppPrefs.regenerateApiKey(context); Engine.restart(context) }) { Text("New key") }
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
                OutlinedButton(shape = RectangleShape, onClick = { Engine.stop(context) }, enabled = engine.serverUp, modifier = Modifier.weight(1f)) { Text("Stop engine") }
                OutlinedButton(shape = RectangleShape, onClick = { Engine.start(context) }, enabled = !engine.serverUp, modifier = Modifier.weight(1f)) { Text("Start engine") }
            }
            engine.info?.let { Text("Engine ${it.version}", fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant) }
            Spacer(Modifier.height(24.dp))
        }
    }
}

/** One parameter: its label and help, the control its type asks for, and whether the person set it. */
@Composable
private fun ParamRow(spec: ParamSpec, cfg: ConfigState, modelName: String?, onSet: (Any?) -> Unit, onReset: () -> Unit) {
    val value = cfg.value(spec.key)
    val mine = spec.key in cfg.userKeys
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        when (spec.type) {
            "bool" -> SwitchRow(spec.label, null, value == true) { onSet(it) }
            "choice" -> {
                val idx = spec.choices.indexOfFirst { it.first == value?.toString() }.coerceAtLeast(0)
                LabeledDropdown(spec.label, spec.choices.map { it.second }, idx) { i -> onSet(spec.choices[i].first) }
            }
            "path" -> {
                Text(spec.label, fontSize = 15.sp)
                Text(modelName ?: value?.toString()?.substringAfterLast('/')?.ifEmpty { "none" } ?: "none", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            else -> NumberOrTextField(spec, value, onSet)
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                if (spec.type != "bool") Hint(spec.help)
                else if (spec.help.isNotEmpty()) Hint(spec.help)
                val tags = listOfNotNull(
                    spec.level,
                    if (mine) "set by you" else null,
                    if (spec.lossy) "changes the reply" else null,
                    if (spec.scope == "session") "applies on reload" else null,
                )
                if (tags.isNotEmpty()) Text(tags.joinToString(" · "), fontSize = 11.sp, color = MaterialTheme.colorScheme.primary)
            }
            if (mine) TextButton(shape = RectangleShape, onClick = onReset, contentPadding = PaddingValues(horizontal = 8.dp)) { Text("Reset", fontSize = 12.sp) }
        }
    }
}

/** A number (or `auto`, where the row accepts it) or free text; committed when the field is left. */
@Composable
private fun NumberOrTextField(spec: ParamSpec, value: Any?, onSet: (Any?) -> Unit) {
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
            error = "between ${paramValueText(spec.min)} and ${paramValueText(spec.max)}"
            return
        }
        error = null
        onSet(v)
    }
    OutlinedTextField(
        value = text,
        onValueChange = { text = it; error = null },
        label = { Text(spec.label) },
        suffix = spec.unit?.let { { Text(it, fontSize = 12.sp) } },
        isError = error != null,
        supportingText = error?.let { { Text(it) } },
        singleLine = spec.type != "text",
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
        keyboardActions = KeyboardActions(onDone = { commit() }),
        modifier = Modifier.fillMaxWidth().onFocusChanged { if (!it.isFocused) commit() },
    )
}

/**
 * The fold the advanced, experimental and debug rows live behind. A tonal bar rather than a plain
 * row: closed, it is the one thing standing between the recommended configuration and the levers
 * that can slow a run down or change the reply, so it reads as a boundary and not as a setting.
 */
@Composable
private fun AdvancedSection(open: Boolean, onToggle: () -> Unit, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxWidth()) {
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Surface(
            color = MaterialTheme.colorScheme.tertiaryContainer,
            shape = MaterialTheme.shapes.small,
            modifier = Modifier.fillMaxWidth().padding(top = 8.dp).clickable(onClick = onToggle),
        ) {
            Row(Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("ADVANCED", fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.sp, color = MaterialTheme.colorScheme.onTertiaryContainer)
                Spacer(Modifier.weight(1f))
                Text(if (open) "hide" else "show", fontSize = 12.sp, color = MaterialTheme.colorScheme.onTertiaryContainer)
            }
        }
        if (open) {
            Text(
                "Every setting, grouped and marked with its level. The groups above are the recommended " +
                    "configuration; the planner sets what it can measure, and the levels beyond basic were " +
                    "measured once or on one device and can slow a run down or change the reply.",
                fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 8.dp, bottom = 4.dp),
            )
            Column(verticalArrangement = Arrangement.spacedBy(10.dp), content = content)
        }
    }
}
