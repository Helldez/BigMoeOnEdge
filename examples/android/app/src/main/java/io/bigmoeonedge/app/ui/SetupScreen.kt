package io.bigmoeonedge.app.ui

import android.os.SystemClock
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.R
import io.bigmoeonedge.app.chat.Chat
import io.bigmoeonedge.app.data.CatalogEntry
import io.bigmoeonedge.app.data.CheckState
import io.bigmoeonedge.app.data.Eta
import io.bigmoeonedge.app.data.FileRole
import io.bigmoeonedge.app.data.ModelDownloader
import io.bigmoeonedge.app.data.RowState
import io.bigmoeonedge.app.data.SafImport
import io.bigmoeonedge.app.data.Setup
import io.bigmoeonedge.app.data.Tone
import io.bigmoeonedge.app.data.checkTone
import io.bigmoeonedge.app.data.gbLabel
import io.bigmoeonedge.app.data.libraryRows
import io.bigmoeonedge.app.data.mayDownload
import io.bigmoeonedge.app.data.resumeLabel
import io.bigmoeonedge.app.engine.Engine
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.Locale

/**
 * The first run (docs/screens.md, "First run"; the web UI's Setup): one path to the first private
 * answer, in three real steps. 1 Pick a model (one that fits in memory beside one larger than it);
 * 2 its download, with the time left; 3 the model loads by itself when the download ends and the
 * conversation opens. The transfers, the listing and the load are the app's existing ones
 * ([ModelDownloader], [Engine]); nothing here decides anything about the engine.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SetupScreen(modifier: Modifier = Modifier, onOpenChat: () -> Unit, onOpenModels: () -> Unit) {
    val context = LocalContext.current
    val engine by Engine.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()

    var progress by remember { mutableStateOf<Map<String, ModelDownloader.Progress>>(emptyMap()) }
    var failure by remember { mutableStateOf<String?>(null) }
    var actionError by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var pick by rememberSaveable { mutableStateOf("") }
    var importing by remember { mutableStateOf<Float?>(null) }

    LaunchedEffect(engine.serverUp) { Engine.refreshModels() }
    LaunchedEffect(Unit) {
        ModelDownloader.events(context).collect { ev ->
            when (ev) {
                is ModelDownloader.Event.InFlight -> progress = ev.downloads
                is ModelDownloader.Event.Completed -> Engine.refreshModels()
                is ModelDownloader.Event.Failed -> { failure = ev.reason; Engine.refreshModels() }
            }
        }
    }

    val models = engine.models
    val ramBytes = (engine.info?.ramTotalMib ?: 0L) * 1024 * 1024
    val catalog = models?.catalog.orEmpty()
    val localModels = remember(models) { FileRole.split(models?.local.orEmpty()) { it.role }.first }
    val rows = remember(models, progress.keys, engine.info?.model?.path) {
        models?.let { libraryRows(it, localModels, progress.keys, engine.info?.model?.path) }.orEmpty()
    }
    // What is already under way comes first: a transfer in flight, then a stopped one, then the suggestion.
    val underWay = rows.firstOrNull { it.entry != null && it.state == RowState.DOWNLOADING }?.entry
        ?: catalog.firstOrNull { it.partial && !it.added }
    val chosen: CatalogEntry? = catalog.firstOrNull { it.id == pick } ?: underWay ?: Setup.recommended(catalog, ramBytes)
    val row = chosen?.let { c -> rows.firstOrNull { it.entry?.id == c.id } }
    val dl = chosen?.let { ModelDownloader.entryProgress(context, it, progress) }
    val choices = remember(catalog) { Setup.choices(catalog) }
    // What the picked model is on this phone, by the server's check of its header: asked when a model
    // becomes the pick, the suggested one included, and kept for the visit.
    val checks = remember { mutableStateMapOf<String, CheckState>() }
    // A paused download stays on the download step, with Resume, unless the person asked to pick again.
    var repick by remember { mutableStateOf(false) }
    val paused = Setup.paused(chosen?.partial == true, row?.state)
    val step = Setup.step(row?.state, engine.loading, paused, repick)

    // The download's pace, from the progress the transfers report, for the time left.
    var samples by remember { mutableStateOf(emptyList<Eta.Sample>()) }
    LaunchedEffect(dl?.downloadedBytes, step) {
        val got = dl?.downloadedBytes ?: 0L
        samples = if (step == 2 && got > 0) Eta.addSample(samples, Eta.Sample(SystemClock.elapsedRealtime(), got)) else emptyList()
    }
    val total = dl?.totalBytes?.takeIf { it > 0 } ?: chosen?.bytes ?: 0L
    val left = Eta.secondsLeft(samples, total)
    val speed = Eta.rate(samples)

    fun download(e: CatalogEntry) = scope.launch {
        repick = false
        busy = true
        actionError = ""
        failure = null
        withContext(Dispatchers.IO) { ModelDownloader.enqueueEntry(context, e) }
            .onSuccess { Engine.refreshModels() }
            .onFailure { actionError = it.message ?: "The download did not start." }
        busy = false
    }

    // Step 3 starts by itself: a downloaded model is loaded and the conversation opens, so the first
    // answer is one question away. Once per visit, and never over a load already running.
    var started by remember { mutableStateOf(false) }
    fun start() {
        val path = row?.local?.path ?: return
        if (started) return
        started = true
        actionError = ""
        scope.launch { Engine.load(path).onFailure { started = false; actionError = it.message ?: "The model did not load." } }
    }
    LaunchedEffect(step, engine.loading, row?.state, actionError) {
        if (step == 3 && !engine.loading && row?.state == RowState.ON_DEVICE && actionError.isEmpty()) start()
    }
    // A load the engine reports as failed is this step's error, with Try again.
    LaunchedEffect(engine.info?.state) {
        if (started && engine.info?.state == "error") {
            started = false
            actionError = engine.info?.error?.ifEmpty { null } ?: "The model did not load."
        }
    }
    // The conversation opens when the model can answer, not when the load request returns.
    LaunchedEffect(started, engine.ready) {
        if (started && engine.ready) {
            Chat.newConversation()
            onOpenChat()
        }
    }

    // A gguf already on the phone: imported into the app's storage, then loaded from Models.
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        importing = -1f
        scope.launch {
            SafImport.importGguf(context, uri) { copied, all -> importing = if (all > 0) (copied.toFloat() / all).coerceIn(0f, 1f) else -1f }
                .onSuccess { importing = null; Engine.refreshModels(); onOpenModels() }
                .onFailure { importing = null; actionError = it.message ?: "The file could not be imported." }
        }
    }

    Scaffold(
        modifier = modifier,
        topBar = {
            TopAppBar(
                title = { Text("Get started") },
                navigationIcon = { Box(Modifier.padding(start = 16.dp)) { BrandMark(wordmark = false) } },
                colors = shellTopBarColors(),
            )
        },
    ) { padding ->
        Column(
            Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Head(engine.info?.ramTotalMib ?: 0, engine.info?.cpuThreads ?: 0, engine.plan?.takeIf { it.available }?.devices.orEmpty().filter { !it.cpu }.map { it.description.ifEmpty { it.name } })
            StepsIndicator(step)
            Surface(color = tokens.surface, shape = SheetCorner, shadowElevation = SheetElevation, modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    when {
                        !engine.serverUp -> EngineDown(engine)
                        models == null -> SkeletonLine()
                        chosen == null -> Row(verticalAlignment = Alignment.CenterVertically) {
                            Text("No model is suggested here. Find one in", modifier = Modifier.weight(1f, fill = false))
                            TextButton(onClick = onOpenModels) { Text("Models") }
                        }
                        step == 1 -> {
                            SheetHeading("Pick a model")
                            // Said before the list, with the two ways around it as buttons: the list is a
                            // suggestion, and a gguf already on the phone must not hide under the sheet.
                            Text("Any gguf model runs here. Use one you already have, find another, or start from a suggestion.", fontSize = 14.sp)
                            OutlinedButton(shape = Corner, onClick = { picker.launch(arrayOf("*/*")) }, modifier = Modifier.fillMaxWidth()) { Text("Use a gguf file on this phone") }
                            OutlinedButton(shape = Corner, onClick = onOpenModels, modifier = Modifier.fillMaxWidth()) { Text("Find a model on Hugging Face") }
                            Text("Suggested for a first run", fontWeight = FontWeight.Bold, fontSize = 15.sp)
                            if (choices.isNotEmpty()) {
                                choices.forEach { e ->
                                    ChoiceCard(
                                        title = e.title, size = gbLabel(e.bytes), checked = checks[e.id] as? CheckState.Done,
                                        selected = chosen.id == e.id, onClick = { pick = e.id },
                                    )
                                }
                            } else {
                                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                    Text(chosen.title, fontWeight = FontWeight.Bold)
                                    Text(gbLabel(chosen.bytes), fontSize = 13.sp, color = tokens.muted)
                                }
                            }
                            val check = checks[chosen.id]
                            LaunchedEffect(chosen.id, engine.serverUp) {
                                if (engine.serverUp && checks[chosen.id] == null) {
                                    val id = chosen.id
                                    checks[id] = CheckState.Pending
                                    checks[id] = Engine.inspectEntry(id).fold({ CheckState.Done(it) }, { CheckState.Failed(it.message ?: "") })
                                }
                            }
                            CheckPanel(chosen.title, check)
                            Button(shape = Corner, colors = primaryButtonColors(), enabled = !busy && engine.serverUp && mayDownload(check), onClick = { download(chosen) }) {
                                Text(if (chosen.partial) resumeLabel(chosen.received, chosen.bytes) else "Download, ${gbLabel(chosen.bytes)}")
                            }
                            Text("It resumes where it stopped if the connection drops.", fontSize = 13.sp, color = tokens.muted)
                            failure?.let { Text("The download stopped. Check the connection and the free space, then resume: $it", fontSize = 13.sp, color = tokens.err) }
                        }
                        step == 2 && paused -> {
                            SheetHeading("Download of ${chosen.title} paused")
                            val frac = if (chosen.bytes > 0) (chosen.received.toFloat() / chosen.bytes).coerceIn(0f, 1f) else 0f
                            LinearProgressIndicator(progress = { frac }, modifier = Modifier.fillMaxWidth().height(8.dp).clip(Corner), trackColor = tokens.surface2)
                            Text(
                                "${gbLabel(chosen.received)} of ${gbLabel(chosen.bytes)} (${(frac * 100).toInt()}%)",
                                style = FigureStyle, color = tokens.muted,
                            )
                            Text("What is already here stays; it goes on from there.", fontSize = 13.sp, color = tokens.muted)
                            failure?.let { Text("The download stopped. Check the connection and the free space, then resume: $it", fontSize = 13.sp, color = tokens.err) }
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                                Button(shape = Corner, colors = primaryButtonColors(), enabled = !busy && engine.serverUp, onClick = { download(chosen) }) {
                                    Text(resumeLabel(chosen.received, chosen.bytes))
                                }
                                OutlinedButton(shape = Corner, onClick = { repick = true }) { Text("Pick another model") }
                            }
                        }
                        step == 2 -> {
                            SheetHeading("Downloading ${chosen.title}")
                            val got = dl?.downloadedBytes ?: chosen.received
                            val frac = if (total > 0) (got.toFloat() / total).coerceIn(0f, 1f) else 0f
                            LinearProgressIndicator(progress = { frac }, modifier = Modifier.fillMaxWidth().height(8.dp).clip(Corner), trackColor = tokens.surface2)
                            Text(
                                "${gbLabel(got)} of ${gbLabel(total)} (${(frac * 100).toInt()}%)",
                                style = FigureStyle, color = tokens.muted,
                            )
                            if (left != null) {
                                Row {
                                    Text("${Eta.humanDuration(left)} left", fontWeight = FontWeight.Bold)
                                    speed?.takeIf { it > 0 }?.let { Text(" · ${rateLabel(it)}/s", color = tokens.muted) }
                                }
                            } else {
                                Text("Measuring the speed…", color = tokens.muted)
                            }
                            Text("You can use the phone meanwhile; the model loads by itself when the download ends.", fontSize = 13.sp, color = tokens.muted)
                            OutlinedButton(shape = Corner, onClick = {
                                scope.launch {
                                    withContext(Dispatchers.IO) { ModelDownloader.cancelEntry(context, chosen) }
                                    Engine.refreshModels() // the kept .part turns the step back into Resume
                                }
                            }) { Text("Pause") }
                        }
                        else -> {
                            SheetHeading("Getting ${chosen.title} ready")
                            Text("The engine measures this phone once, then loads the model. The conversation opens by itself.", color = tokens.muted)
                            if (actionError.isNotEmpty()) {
                                Text(actionError, color = tokens.err, fontSize = 13.sp)
                                Button(shape = Corner, colors = primaryButtonColors(), onClick = { actionError = ""; start() }) { Text("Try again") }
                            } else if (engine.ready && !started) {
                                // Loaded from elsewhere (Models) meanwhile: nothing left to wait for.
                                Button(shape = Corner, colors = primaryButtonColors(), onClick = { Chat.newConversation(); onOpenChat() }) { Text("Go to Chat") }
                            } else {
                                SkeletonLine()
                            }
                        }
                    }
                    if (actionError.isNotEmpty() && step != 3) Text(actionError, color = tokens.err, fontSize = 13.sp)
                    importing?.let { f ->
                        Text("Importing…", fontSize = 13.sp)
                        if (f >= 0f) LinearProgressIndicator(progress = { f }, modifier = Modifier.fillMaxWidth()) else LinearProgressIndicator(Modifier.fillMaxWidth())
                    }
                }
            }
            // The ways around the path, one tap away throughout.
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                // The pick step offers these two itself, as buttons.
                if (step != 1 || chosen == null) {
                    TextButton(onClick = { picker.launch(arrayOf("*/*")) }) { Text("Already have a gguf file? Pick it on the phone", fontSize = 13.sp, textAlign = TextAlign.Center) }
                    TextButton(onClick = onOpenModels) { Text("More models, or one from Hugging Face", fontSize = 13.sp) }
                }
                TextButton(onClick = onOpenChat) { Text("Skip for now", fontSize = 13.sp) }
            }
        }
    }
}

/** The logo on a sheet over the seal's rule, the promise, and the facts the engine knows about the phone. */
@Composable
private fun Head(ramMib: Long, threads: Int, gpus: List<String>) {
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Surface(color = tokens.surface, shape = SheetCorner, shadowElevation = SheetElevation) {
            // As wide as the logo: the rule fills the sheet, and the sheet does not fill the screen.
            Column(Modifier.width(IntrinsicSize.Min), horizontalAlignment = Alignment.CenterHorizontally) {
                Icon(
                    painterResource(R.drawable.ic_logo), contentDescription = null, tint = Color.Unspecified,
                    modifier = Modifier.padding(start = 14.dp, end = 14.dp, top = 14.dp, bottom = 11.dp).size(64.dp),
                )
                Box(Modifier.fillMaxWidth().height(3.dp).background(tokens.seal))
            }
        }
        Spacer(Modifier.height(4.dp))
        Text("A private AI on this phone", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold, textAlign = TextAlign.Center)
        Text(
            "Big models run here, and nothing you write leaves this phone. Setting up is one download.",
            style = MaterialTheme.typography.bodyLarge, color = tokens.muted, textAlign = TextAlign.Center,
        )
        val facts = buildList {
            if (ramMib > 0) add("${Math.round(ramMib / 1024.0)} GiB of memory")
            if (threads > 0) add("$threads processor threads")
            addAll(gpus)
        }
        if (facts.isNotEmpty()) Text(facts.joinToString(" · "), fontSize = 13.sp, color = tokens.muted, textAlign = TextAlign.Center)
    }
}

/** "1 Pick a model · 2 Download · 3 Start": the current number in ink, done ones a green check, later ones outlined. */
@Composable
private fun StepsIndicator(step: Int) {
    val names = listOf("Pick a model", "Download", "Start")
    Row(horizontalArrangement = Arrangement.spacedBy(14.dp), verticalAlignment = Alignment.CenterVertically) {
        names.forEachIndexed { i, name ->
            val n = i + 1
            val now = n == step
            val done = n < step
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                val circle = Modifier.size(24.dp).clip(CircleShape)
                Box(
                    when {
                        now -> circle.background(tokens.primaryFill)
                        done -> circle.border(1.5.dp, tokens.ok, CircleShape)
                        else -> circle.border(1.5.dp, tokens.edge, CircleShape)
                    },
                    contentAlignment = Alignment.Center,
                ) {
                    if (done) Icon(Icons.Filled.Check, contentDescription = "done", tint = tokens.ok, modifier = Modifier.size(14.dp))
                    else Text("$n", fontSize = 12.sp, fontWeight = LabelWeight, color = if (now) tokens.onPrimary else tokens.muted)
                }
                Text(name, fontSize = 14.sp, fontWeight = LabelWeight, color = if (now) tokens.text else tokens.muted)
            }
        }
    }
}

@Composable
private fun SheetHeading(text: String) {
    Text(text, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
}

/** One of the two models offered: the selected one has the accent edge and the accent tint. */
@Composable
private fun ChoiceCard(title: String, size: String, checked: CheckState.Done?, selected: Boolean, onClick: () -> Unit) {
    Surface(
        color = if (selected) tokens.accentTint else tokens.surface,
        shape = Corner,
        border = BorderStroke(if (selected) 2.dp else 1.dp, if (selected) tokens.accent else tokens.line),
        modifier = Modifier.fillMaxWidth().clip(Corner)
            .semantics { this.selected = selected }
            .clickable(role = Role.RadioButton, onClick = onClick),
    ) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(title, fontWeight = FontWeight.Bold)
            Text(size, fontSize = 13.sp, color = tokens.muted)
            checked?.let { Text("Checked", fontSize = 13.sp, fontWeight = FontWeight.SemiBold, color = toneColor(checkTone(it))) }
        }
    }
}

/** The server's answer for the pick, in its words and its tone; how it was checked opens on a tap. */
@Composable
private fun CheckPanel(title: String, check: CheckState?) {
    var how by remember(title) { mutableStateOf(false) }
    Surface(color = tokens.surface, shape = Corner, border = BorderStroke(1.dp, tokens.line), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            when (check) {
                null, CheckState.Pending -> Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    CircularProgressIndicator(Modifier.size(14.dp), strokeWidth = 2.dp)
                    Text("Checking $title on this phone…", fontSize = 14.sp, color = tokens.muted)
                }
                is CheckState.Failed -> {
                    Text("Could not check $title from here. It can still be downloaded.", fontWeight = FontWeight.SemiBold)
                    if (check.why.isNotEmpty()) Text(check.why, fontSize = 13.sp, color = tokens.muted)
                }
                is CheckState.Done -> {
                    val c = check.check
                    val tone = checkTone(check)
                    Text("$title: ${c.sentence}", fontWeight = FontWeight.SemiBold, color = if (tone == Tone.NONE) Color.Unspecified else toneColor(tone))
                    if (c.disk.isNotEmpty()) Text(c.disk, fontSize = 13.sp, color = if (c.downloadable) tokens.muted else tokens.err)
                    TextButton(onClick = { how = !how }, contentPadding = PaddingValues(0.dp)) { Text("How it was checked", fontSize = 13.sp) }
                    if (how) {
                        if (c.headline.isNotEmpty()) Text(c.headline, fontSize = 13.sp)
                        if (c.memory.isNotEmpty()) Text(c.memory, fontSize = 13.sp, color = tokens.muted)
                    }
                }
            }
        }
    }
}

@Composable
private fun SkeletonLine() {
    Box(Modifier.fillMaxWidth().height(16.dp).clip(Corner).background(tokens.surface2))
}

/** Bytes per second as a person reads a download's pace, in the units the app sizes models in. */
private fun rateLabel(bytesPerS: Double): String =
    if (bytesPerS >= 1_000_000) String.format(Locale.US, "%.1f MB", bytesPerS / 1_000_000) else String.format(Locale.US, "%.0f kB", bytesPerS / 1_000)
