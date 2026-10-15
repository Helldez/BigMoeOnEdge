package io.bigmoeonedge.app.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
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
import io.bigmoeonedge.app.data.ModelManager
import io.bigmoeonedge.app.engine.ConfigState
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.engine.ParamSpec
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
    var level by rememberSaveable { mutableStateOf("basic") }

    fun set(key: String, value: Any?) = scope.launch {
        Engine.setConfig(mapOf(key to value)).onFailure { snackbar.showSnackbar(it.message ?: "not saved") }
    }
    fun reset(key: String) = scope.launch {
        Engine.setConfig(emptyMap(), listOf(key)).onFailure { snackbar.showSnackbar(it.message ?: "not reset") }
    }

    Scaffold(
        modifier = modifier,
        topBar = { TopAppBar(title = { Text("Settings") }) },
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
                        TextButton(onClick = { scope.launch { Engine.load(null).onFailure { snackbar.showSnackbar(it.message ?: "reload failed") } } }) { Text("Reload") }
                    }
                }
            }
            if (cfg != null && !cfg.valid) {
                Text("The configuration does not validate: ${cfg.error}", color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
            }

            Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.fillMaxWidth()) {
                ParamSpec.LEVELS.forEach { l ->
                    FilterChip(selected = level == l, onClick = { level = l }, label = { Text(l.replaceFirstChar { it.uppercase() }, fontSize = 12.sp) })
                }
            }

            when {
                !engine.serverUp -> Text("The engine is not running.", color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
                params == null || cfg == null -> Text("Waiting for the engine…", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                else -> ParamSpec.grouped(params).forEach { (groupLabel, rows) ->
                    // A path other than the model is set at launch, not over the API: not a setting here.
                    val shown = rows.filter { ParamSpec.visible(it.level, level) && (it.type != "path" || it.key == "model") }
                    if (shown.isEmpty()) return@forEach
                    Text(groupLabel, fontWeight = FontWeight.Bold, fontSize = 13.sp, color = MaterialTheme.colorScheme.primary)
                    shown.forEach { spec ->
                        ParamRow(spec, cfg, modelName = engine.info?.model?.name, onSet = { set(spec.key, it) }, onReset = { reset(spec.key) })
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
                Text("Address: http://<this phone's IP>:8765/v1 · key:", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(key, fontFamily = FontFamily.Monospace, fontSize = 13.sp, modifier = Modifier.weight(1f))
                    TextButton(onClick = { clipboard.setText(AnnotatedString(key)) }) { Text("Copy") }
                    TextButton(onClick = { key = AppPrefs.regenerateApiKey(context); Engine.restart(context) }) { Text("New key") }
                }
            }
            var csv by remember { mutableStateOf(AppPrefs.metricsCsv(context)) }
            SwitchRow(
                "Metrics CSV",
                "One file per loaded model: per-token timings, faults, cache figures and where memory sat. " +
                    "Read and shared from Metrics.",
                csv,
            ) { on -> csv = on; AppPrefs.setMetricsCsv(context, on) }
            ModelManager.npuUnavailableReason(context)?.let { Hint("NPU prefill: $it") }
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedButton(onClick = { Engine.stop(context) }, enabled = engine.serverUp, modifier = Modifier.weight(1f)) { Text("Stop engine") }
                OutlinedButton(onClick = { Engine.start(context) }, enabled = !engine.serverUp, modifier = Modifier.weight(1f)) { Text("Start engine") }
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
                    if (mine) "set by you" else null,
                    if (spec.lossy) "changes the reply" else null,
                    if (spec.scope == "session") "applies on reload" else null,
                )
                if (tags.isNotEmpty()) Text(tags.joinToString(" · "), fontSize = 11.sp, color = MaterialTheme.colorScheme.primary)
            }
            if (mine) TextButton(onClick = onReset, contentPadding = PaddingValues(horizontal = 8.dp)) { Text("Reset", fontSize = 12.sp) }
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
