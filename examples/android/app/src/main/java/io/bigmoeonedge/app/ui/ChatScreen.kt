package io.bigmoeonedge.app.ui

import androidx.compose.foundation.interaction.DragInteraction
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.chat.Chat
import io.bigmoeonedge.app.chat.ConversationSummary
import io.bigmoeonedge.app.chat.ItemReducer
import io.bigmoeonedge.app.data.breakdown
import io.bigmoeonedge.app.engine.ApiException
import io.bigmoeonedge.app.engine.Decide
import io.bigmoeonedge.app.engine.Decisions
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.engine.contextWarning
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Where the chat's error cards lead: the other screens. */
data class ChatNav(
    val openModels: () -> Unit,
    val openSetting: (String) -> Unit,
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
            ModalDrawerSheet(drawerContainerColor = tokens.surface) {
                Row(
                    Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 16.dp, bottom = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(Modifier.weight(1f)) { BrandMark() }
                    FilledTonalButton(onClick = { Chat.newConversation(); scope.launch { drawer.close() } }, shape = Corner) {
                        Icon(Icons.Filled.Add, contentDescription = null, Modifier.size(18.dp))
                        Spacer(Modifier.width(6.dp))
                        Text("New chat")
                    }
                }
                HorizontalDivider(Modifier.padding(vertical = 8.dp), color = tokens.line)
                SectionTitle("Conversations", Modifier.padding(horizontal = 28.dp, vertical = 4.dp))
                val stamp = remember { SimpleDateFormat("d MMM HH:mm", Locale.getDefault()) }
                if (chat.conversations.isEmpty()) {
                    Text(
                        "None yet. What you send starts one, kept by the engine on this phone.",
                        style = MaterialTheme.typography.bodySmall, color = tokens.muted,
                        modifier = Modifier.padding(horizontal = 28.dp, vertical = 8.dp),
                    )
                }
                LazyColumn(contentPadding = PaddingValues(horizontal = 12.dp, vertical = 4.dp)) {
                    items(chat.conversations, key = { it.id }) { c ->
                        var menu by remember { mutableStateOf(false) }
                        NavigationDrawerItem(
                            selected = c.id == chat.open?.id,
                            onClick = { Chat.open(c.id); scope.launch { drawer.close() } },
                            label = {
                                Column {
                                    Text(c.title.ifEmpty { "New chat" }, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                        val meta = c.updated.takeIf { it > 0 }?.let { stamp.format(Date(it)) }.orEmpty()
                                        Text(meta, fontSize = 11.sp, color = tokens.muted, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                                        ConversationStateChip(c.state)
                                    }
                                }
                            },
                            badge = {
                                Box {
                                    IconButton(onClick = { menu = true }) { Icon(Icons.Filled.MoreVert, contentDescription = "More") }
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
                colors = TopAppBarDefaults.topAppBarColors(containerColor = tokens.bg),
            )
            HorizontalDivider(color = tokens.line)
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
        if (shown.isEmpty() && decision == null && chat.error == null && !chat.sending && !chat.opening) {
            EmptyChat(chat.unsupported)
        }
        LazyColumn(
            state = listState,
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            items(shown) { item ->
                ItemView(item, answerAgain = item.id == againOn, running = chat.running || chat.sending)
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

@Composable
private fun EmptyChat(unsupported: Boolean) {
    Column(Modifier.fillMaxSize().padding(24.dp), verticalArrangement = Arrangement.Center) {
        if (unsupported) {
            Text("This engine keeps no conversations.", style = MaterialTheme.typography.titleMedium)
            Text(
                "The server this app started does not offer conversations. Update the app to a build that carries them.",
                style = MaterialTheme.typography.bodyMedium, color = tokens.muted,
            )
            return@Column
        }
        Text("Ask anything.", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(4.dp))
        Text(
            "The model runs on this phone, and nothing you write leaves it. Load a model from Models, " +
                "then write below.",
            style = MaterialTheme.typography.bodyMedium, color = tokens.muted,
        )
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

/** The composer: the live line while the engine works, then the prompt and Send, or Stop while an answer is written. */
@Composable
private fun Composer() {
    val chat by Chat.state.collectAsStateWithLifecycle()
    val decision by Decisions.state.collectAsStateWithLifecycle()
    val engine by Engine.state.collectAsStateWithLifecycle()
    val focus = LocalFocusManager.current
    var prompt by rememberSaveable { mutableStateOf("") }
    var choose by rememberSaveable { mutableStateOf(false) }
    var optionsText by rememberSaveable { mutableStateOf("") }

    Surface(color = tokens.surface, shadowElevation = 2.dp) {
        Column(Modifier.fillMaxWidth().imePadding()) {
            HorizontalDivider(color = tokens.line)
            LiveLine()
            if (choose) {
                OutlinedTextField(
                    value = optionsText, onValueChange = { optionsText = it },
                    label = { Text("Options, one per line") },
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp).padding(top = 8.dp), minLines = 2, maxLines = 6,
                )
            }
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
                verticalAlignment = Alignment.Bottom,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                var more by remember { mutableStateOf(false) }
                Box {
                    IconButton(onClick = { more = true }) { Icon(Icons.Filled.Add, contentDescription = "More") }
                    DropdownMenu(expanded = more, onDismissRequest = { more = false }) {
                        DropdownMenuItem(
                            text = { Text(if (choose) "Back to a message" else "Choose from options") },
                            leadingIcon = { Icon(Icons.Filled.Build, null) },
                            onClick = { choose = !choose; more = false },
                        )
                    }
                }
                OutlinedTextField(
                    value = prompt,
                    onValueChange = { prompt = it },
                    placeholder = { Text(if (choose) "Question" else "Message") },
                    modifier = Modifier.weight(1f),
                    maxLines = 6,
                    shape = Corner,
                    trailingIcon = if (prompt.isNotEmpty()) ({
                        IconButton(onClick = { prompt = "" }) { Icon(Icons.Filled.Clear, contentDescription = "Clear") }
                    }) else null,
                )
                if (chat.running) {
                    FilledTonalButton(onClick = { Chat.stop() }, shape = Corner, contentPadding = PaddingValues(horizontal = 16.dp)) { Text("Stop") }
                } else {
                    val options = if (choose) Decide.parseOptions(optionsText) else emptyList()
                    FilledIconButton(
                        shape = Corner,
                        onClick = {
                            // Drop focus so the keyboard retracts: the answer streams into the space it covered.
                            focus.clearFocus()
                            if (choose) Decisions.choose(prompt, options) else Chat.send(prompt)
                            prompt = ""
                        },
                        enabled = prompt.isNotBlank() && engine.serverUp && !chat.sending && decision?.running != true &&
                            (!choose || options.size >= 2),
                    ) { Icon(Icons.AutoMirrored.Filled.Send, contentDescription = if (choose) "Choose" else "Send") }
                }
            }
        }
    }
}

/** The rate and where the time goes, one line, while generating and after; the full card is in Metrics. */
@Composable
private fun LiveLine() {
    val engine by Engine.state.collectAsStateWithLifecycle()
    val t = engine.live
    if (t.step == 0 && t.avgTokensPerSecond <= 0) return
    val b = breakdown(t)
    val loc = Locale.US
    val rate = if (b.isAverage) String.format(loc, "%.1f tok/s avg", t.avgTokensPerSecond)
    else String.format(loc, "%.1f tok/s", t.tokensPerSecond)
    val parts = mutableListOf(rate)
    if (b.totalMs > 0) {
        parts += String.format(loc, "compute %.0f%%", 100.0 * b.computeMs / b.totalMs)
        parts += String.format(loc, "flash %.0f%%", 100.0 * b.flashWaitMs / b.totalMs)
    }
    if (t.cacheHitPct >= 0) parts += String.format(loc, "cache %.0f%%", t.cacheHitPct)
    engine.cpuTempC?.let { parts += String.format(loc, "%.0f°C", it) }
    if (engine.generating && t.steps > 0) parts += "${t.step}/${t.steps}"
    Text(
        parts.joinToString(" · "),
        style = FigureStyle, fontSize = 12.sp,
        color = tokens.muted,
        modifier = Modifier.padding(horizontal = 16.dp).padding(top = 6.dp),
        maxLines = 1, overflow = TextOverflow.Ellipsis,
    )
}
