package io.bigmoeonedge.app.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.data.AppPrefs
import io.bigmoeonedge.app.data.ModelDownloader
import io.bigmoeonedge.app.data.downloadSummary
import io.bigmoeonedge.app.data.runningTransfers
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.engine.contextWarning
import io.bigmoeonedge.app.engine.openAppSettings
import io.bigmoeonedge.app.engine.text
import kotlinx.coroutines.flow.filterIsInstance

/**
 * The status line every screen carries (docs/screens.md, "The status line"): what the engine is
 * doing, with the way out when there is one (Models when it has nothing, the app's settings when the
 * system denies it the network), the app's running downloads leading to Models, and, for a loaded
 * model whose context is below what a client of the server sends, while it serves the network, a
 * second line saying so with the setting that raises it (all of it the server's: `model.ctx_advice`),
 * until it is dismissed for that model and context.
 */
@Composable
fun EngineStatusLine(onOpenModels: () -> Unit, onOpenSetting: (String) -> Unit) {
    val engine by Engine.state.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val info = engine.info
    var live by remember { mutableStateOf<Map<String, ModelDownloader.Progress>>(emptyMap()) }
    LaunchedEffect(Unit) {
        ModelDownloader.events(context).filterIsInstance<ModelDownloader.Event.InFlight>().collect { live = it.downloads }
    }
    val catalog = engine.models?.catalog.orEmpty()
    val downloads = remember(live, catalog) { downloadSummary(runningTransfers(catalog, live) { ModelDownloader.entryProgress(context, it, live) }) }

    val toSettings = "Allow" to { openAppSettings(context) }
    val toModels = "Choose one" to onOpenModels
    val (text, action) = when {
        !engine.serverUp && engine.networkDenied -> engine.serverError.orEmpty() to toSettings
        !engine.serverUp -> (engine.serverError?.lineSequence()?.firstOrNull()?.take(120) ?: "Engine not running") to null
        info == null -> "Connecting…" to null
        info.state == "planning" -> "Measuring the phone, then loading…" to null
        info.state == "loading" -> "Loading model…" to null
        info.state == "error" -> "Engine error: ${info.error.take(120)}" to toModels.copy(first = "Models")
        info.model == null -> "No model loaded" to toModels
        else -> {
            // A model running without streaming says so wherever its name is shown.
            val name = info.model.name + if (info.model.streaming) "" else " (not streamed)"
            "$name · ${if (info.generating) "generating" else "ready"}" to null
        }
    }
    Surface(color = MaterialTheme.colorScheme.surfaceVariant, modifier = Modifier.fillMaxWidth()) {
        Row(Modifier.padding(horizontal = 16.dp, vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
            // The denial is the one status a person must read whole to act on: it may wrap.
            Text(text, fontSize = 12.sp, maxLines = if (engine.networkDenied) 3 else 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
            if (downloads != null) {
                TextButton(shape = Corner, onClick = onOpenModels, contentPadding = PaddingValues(horizontal = 8.dp)) { Text(downloads.text, fontSize = 12.sp) }
            }
            if (action != null) TextButton(shape = Corner, onClick = action.second, contentPadding = PaddingValues(horizontal = 8.dp)) { Text(action.first, fontSize = 12.sp) }
        }
    }
    val ctx = contextWarning(info?.model.takeIf { engine.serverUp && info?.ready == true }, engine.params)
    var dismissed by remember { mutableStateOf(AppPrefs.ctxWarningDismissed(context)) }
    if (ctx?.warn == true && ctx.dismissKey != dismissed) {
        Surface(color = warnColor().copy(alpha = 0.15f), modifier = Modifier.fillMaxWidth()) {
            Row(Modifier.padding(horizontal = 16.dp, vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(ctx.text(), fontSize = 12.sp, maxLines = 3, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                TextButton(shape = Corner, onClick = { onOpenSetting(ctx.key) }, contentPadding = PaddingValues(horizontal = 8.dp)) {
                    Text("Raise ${ctx.label}", fontSize = 12.sp)
                }
                IconButton(onClick = {
                    AppPrefs.setCtxWarningDismissed(context, ctx.dismissKey)
                    dismissed = ctx.dismissKey
                }) { Icon(Icons.Filled.Close, contentDescription = "Dismiss the context warning") }
            }
        }
    }
}
