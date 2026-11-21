package io.bigmoeonedge.app.ui

import android.content.ActivityNotFoundException
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.interaction.DragInteraction
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.FileProvider
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.R
import io.bigmoeonedge.app.chat.Chat
import io.bigmoeonedge.app.chat.ConversationSummary
import io.bigmoeonedge.app.chat.ImageAttachments
import io.bigmoeonedge.app.chat.ItemReducer
import io.bigmoeonedge.app.chat.Words
import io.bigmoeonedge.app.engine.ApiException
import io.bigmoeonedge.app.engine.Decide
import io.bigmoeonedge.app.engine.Decisions
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.engine.contextWarning
import io.bigmoeonedge.app.engine.planFigures
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Where the chat's error cards and an answer's figures lead: the other screens. */
data class ChatNav(
    val openModels: () -> Unit,
    val openSetting: (String) -> Unit,
    val openPlan: () -> Unit = {},
    val openMetrics: () -> Unit = {},
)

/**
 * The chat: the conversations (the server's, docs/chat.md; plain chat on a phone) in a drawer;
 * the open one in the middle as its messages; the composer at the bottom where a thumb reaches it.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChatScreen(modifier: Modifier = Modifier, nav: ChatNav) {
    val chat by Chat.state.collectAsStateWithLifecycle()
    val drawer = rememberDrawerState(DrawerValue.Closed)
    val scope = rememberCoroutineScope()
    var confirmDelete by remember { mutableStateOf<ConversationSummary?>(null) }
    var renaming by remember { mutableStateOf<ConversationSummary?>(null) }
    LaunchedEffect(Unit) { Chat.refreshConversations() }

    ModalNavigationDrawer(
        drawerState = drawer,
        drawerContent = {
            // The drawer is the shell: ink, its words on-shell, the open conversation's row lit.
            ModalDrawerSheet(drawerContainerColor = tokens.shell, drawerContentColor = tokens.onShell) {
                Row(
                    Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 16.dp, bottom = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(Modifier.weight(1f)) { BrandMark() }
                    FilledTonalButton(
                        onClick = { Chat.newConversation(); scope.launch { drawer.close() } }, shape = Corner,
                        colors = ButtonDefaults.filledTonalButtonColors(containerColor = tokens.shellHi, contentColor = tokens.onShell),
                    ) {
                        Icon(Icons.Filled.Add, contentDescription = null, Modifier.size(18.dp))
                        Spacer(Modifier.width(6.dp))
                        Text("New chat")
                    }
                }
                HorizontalDivider(Modifier.padding(vertical = 8.dp), color = tokens.shellHi)
                Text(
                    "Conversations", style = MaterialTheme.typography.titleSmall, color = tokens.onShellMuted,
                    modifier = Modifier.padding(horizontal = 28.dp, vertical = 4.dp),
                )
                val stamp = remember { SimpleDateFormat("d MMM HH:mm", Locale.getDefault()) }
                if (chat.conversations.isEmpty()) {
                    Text(
                        "None yet. What you send starts one, kept by the engine on this phone.",
                        style = MaterialTheme.typography.bodySmall, color = tokens.onShellMuted,
                        modifier = Modifier.padding(horizontal = 28.dp, vertical = 8.dp),
                    )
                }
                val rowColours = NavigationDrawerItemDefaults.colors(
                    selectedContainerColor = tokens.shellHi,
                    unselectedContainerColor = Color.Transparent,
                    selectedTextColor = tokens.onShell,
                    unselectedTextColor = tokens.onShell,
                    selectedBadgeColor = tokens.onShellMuted,
                    unselectedBadgeColor = tokens.onShellMuted,
                )
                LazyColumn(contentPadding = PaddingValues(horizontal = 12.dp, vertical = 4.dp)) {
                    items(chat.conversations, key = { it.id }) { c ->
                        var menu by remember { mutableStateOf(false) }
                        NavigationDrawerItem(
                            colors = rowColours,
                            selected = c.id == chat.open?.id,
                            onClick = { Chat.open(c.id); scope.launch { drawer.close() } },
                            label = {
                                Column {
                                    Text(c.title.ifEmpty { "New chat" }, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                        val meta = c.updated.takeIf { it > 0 }?.let { stamp.format(Date(it)) }.orEmpty()
                                        Text(meta, fontSize = 11.sp, color = tokens.onShellMuted, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                                        ConversationStateChip(c.state)
                                    }
                                }
                            },
                            badge = {
                                Box {
                                    IconButton(onClick = { menu = true }) { Icon(Icons.Filled.MoreVert, contentDescription = "More", tint = tokens.onShellMuted) }
                                    DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                                        DropdownMenuItem(text = { Text("Rename") }, leadingIcon = { Icon(Icons.Filled.Edit, null) }, onClick = { menu = false; renaming = c })
                                        DropdownMenuItem(text = { Text("Delete") }, leadingIcon = { Icon(Icons.Filled.Delete, null) }, onClick = { menu = false; confirmDelete = c })
                                    }
                                }
                            },
                        )
                    }
                }
            }
        },
    ) {
        Column(modifier.fillMaxSize()) {
            TopAppBar(
                title = {
                    Text(chat.open?.title?.ifEmpty { null } ?: "New chat", maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.titleMedium)
                },
                navigationIcon = {
                    IconButton(onClick = { scope.launch { drawer.open() } }) { Icon(Icons.Filled.Menu, contentDescription = "Conversations") }
                },
                actions = {
                    if (chat.open != null) {
                        IconButton(onClick = { Chat.newConversation() }) { Icon(Icons.Filled.Add, contentDescription = "New chat") }
                    }
                },
                colors = shellTopBarColors(),
            )
            if (chat.items.isNotEmpty() || chat.running || chat.sending) ConversationStatusLine()
            Transcript(Modifier.weight(1f), nav)
            Composer()
        }
    }

    confirmDelete?.let { c ->
        AlertDialog(
            onDismissRequest = { confirmDelete = null },
            title = { Text("Delete this conversation?") },
            text = { Text("\"${c.title.ifEmpty { "New chat" }}\" and everything in it is removed from the engine.") },
            confirmButton = { TextButton(onClick = { Chat.delete(c.id); confirmDelete = null }) { Text("Delete", color = tokens.err) } },
            dismissButton = { TextButton(onClick = { confirmDelete = null }) { Text("Cancel") } },
        )
    }
    renaming?.let { c ->
        var title by remember(c.id) { mutableStateOf(c.title) }
        AlertDialog(
            onDismissRequest = { renaming = null },
            title = { Text("Rename") },
            text = { OutlinedTextField(value = title, onValueChange = { title = it }, singleLine = true, modifier = Modifier.fillMaxWidth()) },
            confirmButton = { TextButton(onClick = { Chat.rename(c.id, title.trim()); renaming = null }, enabled = title.isNotBlank()) { Text("Save") } },
            dismissButton = { TextButton(onClick = { renaming = null }) { Text("Cancel") } },
        )
    }
}

@Composable
fun ConversationStateChip(state: String) {
    when (state) {
        ConversationSummary.STATE_RUNNING -> Chip("running", tokens.accentInk)
        ConversationSummary.STATE_ERROR -> Chip("error", tokens.err)
    }
}

@Composable
private fun Transcript(modifier: Modifier, nav: ChatNav) {
    val chat by Chat.state.collectAsStateWithLifecycle()
    val decision by Decisions.state.collectAsStateWithLifecycle()
    // Only the context advice, not every token event: the transcript is the heaviest thing on screen.
    val ctxAdvice by remember { Engine.state.map { contextWarning(it.info?.model, it.params) }.distinctUntilChanged() }.collectAsStateWithLifecycle(null)
    // The rate the plan predicts for the loaded model, read against each answer's own.
    val expectedTokS by remember {
        Engine.state.map { s -> s.plan?.takeIf { it.available && it.error.isEmpty() }?.let { planFigures(it).predictedTokS } }.distinctUntilChanged()
    }.collectAsStateWithLifecycle(null)
    val listState = rememberLazyListState()
    val shown = chat.items
    val againOn = remember(shown) { ItemReducer.answerAgainOn(shown) }
    val working = chat.running && shown.lastOrNull()?.streaming != true
    val total = shown.size + (if (working || chat.sending) 1 else 0) + (if (decision != null) 1 else 0) + (if (chat.error != null) 1 else 0)
    val lastText = chat.items.lastOrNull()?.let { it.text.length + it.reasoning.length } ?: 0

    // Follow the tail only while the person is parked at the bottom: scrolling back to re-read a
    // long answer must not fight a per-token scroll command.
    var followTail by remember { mutableStateOf(true) }
    val atBottom by remember {
        derivedStateOf {
            val info = listState.layoutInfo
            val last = info.visibleItemsInfo.lastOrNull()
            last == null || (last.index == info.totalItemsCount - 1 && last.offset + last.size <= info.viewportEndOffset)
        }
    }
    LaunchedEffect(listState) {
        listState.interactionSource.interactions.collect { if (it is DragInteraction.Start) followTail = false }
    }
    LaunchedEffect(listState) {
        snapshotFlow { listState.isScrollInProgress }.collect { scrolling -> if (!scrolling) followTail = atBottom }
    }
    LaunchedEffect(total, lastText, followTail) {
        if (followTail && total > 0) runCatching { listState.scrollToItem(total - 1, Int.MAX_VALUE) }
    }

    Box(modifier.fillMaxWidth()) {
        if (chat.opening) {
            LinearProgressIndicator(Modifier.fillMaxWidth().align(Alignment.TopCenter))
        }
        val empty = shown.isEmpty() && decision == null && chat.error == null && !chat.sending && !chat.opening
        LazyColumn(
            state = listState,
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            items(shown) { item ->
                ItemView(
                    item, answerAgain = item.id == againOn, running = chat.running || chat.sending,
                    figures = chat.figures[item.id], expectedTokS = expectedTokS, nav = nav,
                )
            }
            if (working || chat.sending) {
                item(key = "working") {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp, color = tokens.accent)
                        Text(if (chat.sending) "Sending…" else "Working…", style = MaterialTheme.typography.bodySmall, color = tokens.muted)
                    }
                }
            }
            decision?.let { d -> item(key = "decision") { DecisionCard(d, nav.openModels) } }
            chat.error?.let { err ->
                item(key = "error") {
                    ErrorCard(err, chat.errorCode, ctxAdvice?.let { it.nCtx to (it.key to it.label) }, nav)
                }
            }
        }
        // Drawn over the (empty) list, so the questions to try take the taps.
        if (empty) EmptyChat(chat.unsupported)
        if (!followTail && !atBottom && total > 0) {
            val scope = rememberCoroutineScope()
            FilledTonalButton(
                shape = Corner,
                onClick = { scope.launch { listState.animateScrollToItem(total - 1, Int.MAX_VALUE) } },
                modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 12.dp),
            ) { Text("Jump to latest") }
        }
    }
}

/**
 * The empty conversation: the logo on a white sheet over the seal's yellow rule, the status in words
 * under it (when all is ready, the promise that nothing leaves the phone), and the question.
 */
@Composable
private fun EmptyChat(unsupported: Boolean) {
    val engine by Engine.state.collectAsStateWithLifecycle()
    val chat by Chat.state.collectAsStateWithLifecycle()
    BoxWithConstraints(Modifier.fillMaxSize()) {
    Column(
        Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).heightIn(min = maxHeight).padding(24.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        if (unsupported) {
            Text("This engine keeps no conversations.", style = MaterialTheme.typography.titleMedium)
            Text(
                "The server this app started does not offer conversations. Update the app to a build that carries them.",
                style = MaterialTheme.typography.bodyMedium, color = tokens.muted, textAlign = TextAlign.Center,
            )
            return@Column
        }
        val status = rememberConversationStatus(greet = true)
        Surface(color = tokens.surface, shape = SheetCorner, shadowElevation = SheetElevation) {
            // As wide as the logo: the rule fills the sheet, and the sheet does not fill the screen.
            Column(Modifier.width(IntrinsicSize.Min), horizontalAlignment = Alignment.CenterHorizontally) {
                StatusLogo(status.mood, 64.dp, Modifier.padding(start = 14.dp, end = 14.dp, top = 14.dp, bottom = 11.dp))
                Box(Modifier.fillMaxWidth().height(3.dp).background(tokens.seal))
            }
        }
        Spacer(Modifier.height(16.dp))
        Text(
            status.text, style = MaterialTheme.typography.bodyLarge, fontWeight = LabelWeight, color = statusColour(status.mood),
            textAlign = TextAlign.Center, modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
        )
        Spacer(Modifier.height(20.dp))
        Text(Words.WELCOME, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold, textAlign = TextAlign.Center)
        Spacer(Modifier.height(8.dp))
        Text(Words.WELCOME_LINE, style = MaterialTheme.typography.bodyLarge, color = tokens.muted, textAlign = TextAlign.Center)
        // Questions to try, once a model can answer them: each is sent as it reads.
        if (engine.ready && !chat.sending && !chat.running) {
            Spacer(Modifier.height(20.dp))
            Column(verticalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                Words.TRIES.forEach { q ->
                    Surface(
                        color = tokens.surface, shape = Corner, border = BorderStroke(1.dp, tokens.line), shadowElevation = 1.dp,
                        modifier = Modifier.fillMaxWidth().clip(Corner).clickable { Chat.send(q) },
                    ) {
                        Text(q, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.padding(horizontal = 14.dp, vertical = 12.dp))
                    }
                }
            }
        }
    }
    }
}

/** Why a send or a run failed, decided on by its code; the fix sits beside it. */
@Composable
private fun ErrorCard(err: String, code: String, ctx: Pair<Int, Pair<String, String>>?, nav: ChatNav) {
    BmoeCard(colour = tokens.errTint, padding = 12.dp) {
        Text(err, color = tokens.text, style = MaterialTheme.typography.bodyMedium)
        val contextFull = code == ApiException.CODE_CONTEXT_FULL
        if (contextFull) {
            Text(
                "The conversation has filled the model's context" + (ctx?.let { " of ${it.first} tokens" } ?: "") + ".",
                style = MaterialTheme.typography.bodySmall, color = tokens.muted,
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
            Spacer(Modifier.weight(1f))
            when {
                contextFull -> {
                    if (ctx != null) TextButton(onClick = { nav.openSetting(ctx.second.first) }) { Text("Raise ${ctx.second.second}") }
                    TextButton(onClick = { Chat.newConversation() }) { Text("New chat") }
                }
                code == ApiException.CODE_NO_MODEL -> TextButton(onClick = nav.openModels) { Text("Models") }
            }
            TextButton(onClick = { Chat.clearError() }) { Text("Dismiss") }
        }
    }
}

/** A decision's answer: every option with the probability the model put on it, the pick in bold. */
@Composable
private fun DecisionCard(d: io.bigmoeonedge.app.engine.DecisionUi, onOpenModels: () -> Unit) {
    BmoeCard(padding = 12.dp) {
        Text("Choose: " + d.question, style = MaterialTheme.typography.titleSmall)
        when {
            d.running -> Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                Text("Reading the options…", style = MaterialTheme.typography.bodySmall, color = tokens.muted)
                Spacer(Modifier.weight(1f))
                TextButton(onClick = { Decisions.stop() }) { Text("Stop") }
            }
            d.error != null -> {
                Text(d.error, color = tokens.err, style = MaterialTheme.typography.bodySmall)
                if (d.errorCode == ApiException.CODE_NO_MODEL) TextButton(onClick = onOpenModels) { Text("Models") }
            }
            else -> d.scores.forEachIndexed { i, c ->
                val picked = i == d.best
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        "${c.label}) ${c.text}", style = MaterialTheme.typography.bodyMedium,
                        fontWeight = if (picked) FontWeight.SemiBold else FontWeight.Normal,
                        maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f),
                    )
                    LinearProgressIndicator(
                        progress = { c.prob.toFloat().coerceIn(0f, 1f) }, color = if (picked) tokens.accent else tokens.muted,
                        trackColor = tokens.surface2, modifier = Modifier.width(72.dp).height(6.dp),
                    )
                    Text(String.format(Locale.US, "%.0f%%", 100.0 * c.prob), style = FigureStyle, modifier = Modifier.width(40.dp))
                }
            }
        }
        if (d.metrics.isNotEmpty()) Text(d.metrics, style = CodeStyle.copy(fontSize = 11.sp), color = tokens.muted)
        if (!d.running) Row { Spacer(Modifier.weight(1f)); TextButton(onClick = { Decisions.dismiss() }) { Text("Dismiss") } }
    }
}

/**
 * The composer: the live line while the engine works, then the images going with the next message,
 * the prompt and Send, or Stop while an answer is written. The "+" menu holds the other ways in: a
 * question with options to choose from, an image from the photos, a photo taken now.
 */
@Composable
private fun Composer() {
    val chat by Chat.state.collectAsStateWithLifecycle()
    val decision by Decisions.state.collectAsStateWithLifecycle()
    val engine by Engine.state.collectAsStateWithLifecycle()
    val focus = LocalFocusManager.current
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var prompt by rememberSaveable { mutableStateOf("") }
    var choose by rememberSaveable { mutableStateOf(false) }
    var optionsText by rememberSaveable { mutableStateOf("") }
    // The images going with the next message, as the data URLs they will be sent as. Not saved
    // across a process death: a few photos are more than a saved state may hold.
    var images by remember { mutableStateOf(listOf<String>()) }
    var imageError by remember { mutableStateOf("") }
    // Where the camera writes: kept across the camera activity, which may have this one killed meanwhile.
    var cameraUri by rememberSaveable { mutableStateOf<String?>(null) }

    // The bytes are read off the main thread (a photo is megabytes); the rules (how many, how
    // large, images only: the web composer's, in ImageAttachments) and every write of the screen's
    // state run on it. Of a pick with several refusals the first reason stays under the thumbnails,
    // and nothing past the count limit is read: the rule the three UIs share.
    fun attach(uris: List<Uri>, deleteAfter: Boolean = false) {
        if (uris.isEmpty()) return
        scope.launch {
            imageError = ""
            for (uri in uris) {
                if (images.size >= ImageAttachments.MAX_IMAGES) {
                    if (imageError.isEmpty()) imageError = ImageAttachments.accept(ImageAttachments.DEFAULT_MIME, 0, images.size).orEmpty()
                    if (deleteAfter) withContext(Dispatchers.IO) { runCatching { ctx.contentResolver.delete(uri, null, null) } }
                    break
                }
                val (mime, bytes) = withContext(Dispatchers.IO) {
                    val mime = ctx.contentResolver.getType(uri)
                    val bytes = runCatching { ctx.contentResolver.openInputStream(uri)?.use { readUpTo(it, ImageAttachments.MAX_BYTES + 1) } }.getOrNull()
                    if (deleteAfter) runCatching { ctx.contentResolver.delete(uri, null, null) }
                    mime to bytes
                }
                val why = if (bytes == null) "The image could not be read" else ImageAttachments.accept(mime, bytes.size.toLong(), images.size)
                if (why != null) {
                    if (imageError.isEmpty()) imageError = why
                    continue
                }
                images = images + ImageAttachments.dataUrl(mime, bytes!!)
            }
        }
    }

    // The system photo picker: several at once, image types only, and no storage permission.
    val pickImages = rememberLauncherForActivityResult(ActivityResultContracts.PickMultipleVisualMedia(ImageAttachments.MAX_IMAGES)) { uris ->
        attach(uris)
    }
    // The camera app writes into this app's cache through the FileProvider; the bytes are read
    // into the message and the file dropped. The contract needs no CAMERA permission.
    val takePicture = rememberLauncherForActivityResult(ActivityResultContracts.TakePicture()) { taken ->
        val uri = cameraUri?.let(Uri::parse)
        cameraUri = null
        if (uri != null) {
            if (taken) attach(listOf(uri), deleteAfter = true) else runCatching { ctx.contentResolver.delete(uri, null, null) }
        }
    }
    fun capture() {
        val dir = File(ctx.cacheDir, "camera").apply { mkdirs() }
        val uri = FileProvider.getUriForFile(ctx, "${ctx.packageName}.fileprovider", File(dir, "capture-${System.currentTimeMillis()}.jpg"))
        cameraUri = uri.toString()
        try {
            takePicture.launch(uri)
        } catch (_: ActivityNotFoundException) {
            cameraUri = null
            imageError = "No camera app is installed"
        }
    }

    // A white sheet with a field's edge, lifted off the desk: where the next message is written.
    Surface(
        color = tokens.surface, shape = Corner, shadowElevation = SheetElevation, border = BorderStroke(1.dp, tokens.edge),
        modifier = Modifier.fillMaxWidth().imePadding().padding(horizontal = 12.dp).padding(top = 4.dp, bottom = 10.dp),
    ) {
        Column(Modifier.fillMaxWidth()) {
            if (choose) {
                OutlinedTextField(
                    value = optionsText, onValueChange = { optionsText = it },
                    label = { Text("Options, one per line") },
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp).padding(top = 8.dp), minLines = 2, maxLines = 6,
                )
            }
            if (images.isNotEmpty()) {
                Row(
                    Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 12.dp).padding(top = 8.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    images.forEachIndexed { i, src ->
                        Box {
                            ImageThumb(src, ComposerThumb)
                            FilledIconButton(
                                onClick = { images = images.filterIndexed { k, _ -> k != i } },
                                modifier = Modifier.align(Alignment.TopEnd).padding(2.dp).size(20.dp),
                                colors = IconButtonDefaults.filledIconButtonColors(containerColor = tokens.surface, contentColor = tokens.text),
                            ) { Icon(Icons.Filled.Close, contentDescription = "Remove image", Modifier.size(14.dp)) }
                        }
                    }
                }
            }
            // While a model loads, the composer says when it can be used rather than that nothing is loaded.
            if (engine.loading) {
                Text(Words.COMPOSER_LOADING, style = MaterialTheme.typography.bodySmall, color = tokens.muted, modifier = Modifier.padding(horizontal = 16.dp).padding(top = 8.dp))
            }
            if (imageError.isNotEmpty()) {
                Text(imageError, color = tokens.err, style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(horizontal = 16.dp).padding(top = 6.dp))
            }
            var more by remember { mutableStateOf(false) }
            // Whether an image can go: the loaded model reads them (GET /api/info, model.vision).
            // The entries stay, disabled, with the reason: the feature is learnt by seeing it.
            val model = engine.info?.model
            val imageWhy = when {
                choose -> "An image goes with a message, not with a choice"
                !engine.ready || model == null -> "No model is loaded: one loaded with its projector file reads images"
                !model.vision -> "The loaded model does not read images: load one with its projector file"
                else -> null
            }
            // The sheet is the field's edge: the field itself draws none. It has the sheet's whole
            // width, with the controls on a line below, as the web and iOS composers lay it out: on a
            // phone, + and Send (or Stop) beside it left the words a third of the screen.
            TextField(
                value = prompt,
                onValueChange = { prompt = it },
                placeholder = { Text(if (choose) "Question" else "Message") },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 4.dp).padding(top = 4.dp),
                maxLines = 6,
                shape = Corner,
                colors = TextFieldDefaults.colors(
                    focusedContainerColor = Color.Transparent,
                    unfocusedContainerColor = Color.Transparent,
                    disabledContainerColor = Color.Transparent,
                    focusedIndicatorColor = Color.Transparent,
                    unfocusedIndicatorColor = Color.Transparent,
                    disabledIndicatorColor = Color.Transparent,
                ),
            )
            Row(
                Modifier.fillMaxWidth().padding(start = 4.dp, end = 12.dp, bottom = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box {
                    IconButton(onClick = { more = true }) { Icon(Icons.Filled.Add, contentDescription = "More") }
                    DropdownMenu(expanded = more, onDismissRequest = { more = false }) {
                        DropdownMenuItem(
                            text = { Text(if (choose) "Back to a message" else "Choose from options") },
                            leadingIcon = { Icon(painterResource(R.drawable.ic_checklist), null) },
                            onClick = { choose = !choose; more = false },
                        )
                        HorizontalDivider(color = tokens.line)
                        DropdownMenuItem(
                            text = { Text("Image") },
                            leadingIcon = { Icon(painterResource(R.drawable.ic_image), null) },
                            enabled = imageWhy == null,
                            onClick = { more = false; pickImages.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)) },
                        )
                        DropdownMenuItem(
                            text = { Text("Take a photo") },
                            leadingIcon = { Icon(painterResource(R.drawable.ic_photo_camera), null) },
                            enabled = imageWhy == null,
                            onClick = { more = false; capture() },
                        )
                        if (imageWhy != null) {
                            Text(
                                imageWhy, style = MaterialTheme.typography.bodySmall, color = tokens.muted,
                                modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp).widthIn(max = 240.dp),
                            )
                        }
                    }
                }
                Spacer(Modifier.weight(1f))
                if (chat.running) {
                    FilledTonalButton(onClick = { Chat.stop() }, shape = Corner, contentPadding = PaddingValues(horizontal = 16.dp)) { Text("Stop") }
                } else {
                    val options = if (choose) Decide.parseOptions(optionsText) else emptyList()
                    FilledIconButton(
                        shape = CircleShape,
                        colors = primaryIconButtonColors(),
                        onClick = {
                            // Drop focus so the keyboard retracts: the answer streams into the space it covered.
                            focus.clearFocus()
                            if (choose) {
                                Decisions.choose(prompt, options)
                                prompt = ""
                            } else {
                                val text = prompt
                                val attached = images
                                prompt = ""
                                images = emptyList()
                                imageError = ""
                                // Not taken: what was to go comes back, and the error card says why.
                                // The store answers from its own thread; the screen's state is written on Main.
                                Chat.send(text, attached) { ok ->
                                    if (!ok) scope.launch {
                                        prompt = text
                                        images = attached
                                    }
                                }
                            }
                        },
                        enabled = (prompt.isNotBlank() || (images.isNotEmpty() && !choose)) && engine.serverUp && !chat.sending &&
                            decision?.running != true && (!choose || options.size >= 2),
                    ) { Icon(Icons.AutoMirrored.Filled.Send, contentDescription = if (choose) "Choose" else "Send") }
                }
            }
        }
    }
}

/** The thumbnail of an image waiting to be sent: smaller than in the transcript, it sits above the keyboard. */
private val ComposerThumb = 64.dp

/** Up to [limit] bytes of [input]: a file past the largest image allowed is refused without being read whole. */
private fun readUpTo(input: InputStream, limit: Long): ByteArray {
    val out = ByteArrayOutputStream()
    val buf = ByteArray(64 * 1024)
    var left = limit
    while (left > 0) {
        val n = input.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
        if (n < 0) break
        out.write(buf, 0, n)
        left -= n
    }
    return out.toByteArray()
}
