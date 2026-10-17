package io.bigmoeonedge.app.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.DragInteraction
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.data.breakdown
import io.bigmoeonedge.app.engine.Chat
import io.bigmoeonedge.app.engine.ChatMessage
import io.bigmoeonedge.app.engine.Decide
import io.bigmoeonedge.app.engine.Engine
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The chat: the conversations in a drawer, the current one in the middle, the prompt at the
 * bottom where a thumb reaches it. An answer streams in with its thinking span above it; the
 * figures under a finished answer are the turn's own.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChatScreen(modifier: Modifier = Modifier, onOpenModels: () -> Unit) {
    val chat by Chat.state.collectAsStateWithLifecycle()
    val drawer = rememberDrawerState(DrawerValue.Closed)
    val scope = rememberCoroutineScope()

    ModalNavigationDrawer(
        drawerState = drawer,
        drawerContent = {
            ModalDrawerSheet {
                Row(
                    Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 16.dp, bottom = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(Modifier.weight(1f)) { BrandMark() }
                    TextButton(shape = RectangleShape, 
                        onClick = { Chat.newConversation(); scope.launch { drawer.close() } },
                        enabled = !chat.streaming,
                    ) {
                        Icon(Icons.Filled.Add, contentDescription = null)
                        Spacer(Modifier.width(4.dp))
                        Text("New")
                    }
                }
                HorizontalDivider()
                val stamp = remember { SimpleDateFormat("d MMM HH:mm", Locale.getDefault()) }
                LazyColumn {
                    items(chat.conversations, key = { it.id }) { c ->
                        ListItem(
                            headlineContent = { Text(c.title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                            supportingContent = {
                                Text(
                                    listOf(stamp.format(Date(c.updatedAt)), "${c.messageCount} messages", c.model)
                                        .filter { it.isNotEmpty() }.joinToString(" · "),
                                    fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
                                )
                            },
                            trailingContent = {
                                IconButton(onClick = { Chat.delete(c.id) }, enabled = !chat.streaming || c.id != chat.current.id) {
                                    Icon(Icons.Filled.Delete, contentDescription = "Delete")
                                }
                            },
                            colors = ListItemDefaults.colors(
                                containerColor = if (c.id == chat.current.id) MaterialTheme.colorScheme.secondaryContainer
                                else MaterialTheme.colorScheme.surface,
                            ),
                            modifier = Modifier.clickable(enabled = !chat.streaming) {
                                Chat.select(c.id)
                                scope.launch { drawer.close() }
                            },
                        )
                    }
                }
            }
        },
    ) {
        Column(modifier.fillMaxSize()) {
            TopAppBar(
                title = { Text(chat.current.title.ifEmpty { "New chat" }, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                navigationIcon = {
                    IconButton(onClick = { scope.launch { drawer.open() } }) {
                        Icon(Icons.Filled.Menu, contentDescription = "Conversations")
                    }
                },
                actions = {
                    if (chat.current.messages.isNotEmpty()) {
                        TextButton(shape = RectangleShape, onClick = { Chat.newConversation() }, enabled = !chat.streaming) { Text("New chat") }
                    }
                },
            )
            EngineStatusLine(onOpenModels)
            Transcript(Modifier.weight(1f), onOpenModels)
            PromptBar()
        }
    }
}

/** One line that says what the engine is doing, with the way to the Models screen when it has nothing. */
@Composable
private fun EngineStatusLine(onOpenModels: () -> Unit) {
    val engine by Engine.state.collectAsStateWithLifecycle()
    val info = engine.info
    val (text, action) = when {
        !engine.serverUp -> (engine.serverError?.lineSequence()?.firstOrNull()?.take(120) ?: "Engine not running") to null
        info == null -> "Connecting…" to null
        info.state == "planning" -> "Measuring the phone, then loading…" to null
        info.state == "loading" -> "Loading model…" to null
        info.state == "error" -> "Engine error: ${info.error.take(120)}" to null
        info.model == null -> "No model loaded" to "Choose one"
        info.generating -> "${info.model.name} · generating" to null
        else -> "${info.model.name} · ready" to null
    }
    Surface(color = MaterialTheme.colorScheme.surfaceVariant, modifier = Modifier.fillMaxWidth()) {
        Row(Modifier.padding(horizontal = 16.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(text, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
            if (action != null) TextButton(shape = RectangleShape, onClick = onOpenModels, contentPadding = PaddingValues(horizontal = 8.dp)) { Text(action, fontSize = 12.sp) }
        }
    }
}

@Composable
private fun Transcript(modifier: Modifier, onOpenModels: () -> Unit) {
    val chat by Chat.state.collectAsStateWithLifecycle()
    val listState = rememberLazyListState()
    val messages = chat.current.messages
    val liveShown = chat.streaming
    val total = messages.size + (if (liveShown) 1 else 0) + (if (chat.error != null) 1 else 0)

    // Follow the tail only while the user is parked at the bottom. A long answer streams for a
    // long time, and scrolling back to re-read it must not fight a per-token scroll command.
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
    LaunchedEffect(total, chat.answer.length, chat.reasoning.length, followTail) {
        if (followTail && total > 0) runCatching { listState.scrollToItem(total - 1, Int.MAX_VALUE) }
    }

    Box(modifier.fillMaxWidth()) {
        if (messages.isEmpty() && !liveShown && chat.error == null) {
            Column(Modifier.fillMaxSize().padding(24.dp), verticalArrangement = Arrangement.Center) {
                Text("Ask anything.", fontSize = 18.sp, fontWeight = FontWeight.Medium)
                Text(
                    "The model runs on this phone, streamed from storage. Load one from Models if none is " +
                        "loaded, then type below.",
                    fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        LazyColumn(
            state = listState,
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            itemsIndexed(messages) { i, m ->
                MessageView(m, isLast = i == messages.lastIndex, streaming = chat.streaming)
            }
            if (liveShown) {
                item(key = "live") {
                    if (chat.answer.isEmpty() && chat.reasoning.isEmpty()) {
                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                            CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                            Text("Reading the prompt…", fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    } else {
                        MessageView(ChatMessage("assistant", chat.answer, chat.reasoning), isLast = true, streaming = true, reasoningOpen = true)
                    }
                }
            }
            chat.error?.let { err ->
                item(key = "error") {
                    ElevatedCard(Modifier.fillMaxWidth()) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Text(err, color = MaterialTheme.colorScheme.error, fontSize = 13.sp)
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                if (chat.contextFull) {
                                    Text(
                                        "The conversation has filled the model's context.",
                                        fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                                        modifier = Modifier.weight(1f),
                                    )
                                    TextButton(shape = RectangleShape, onClick = { Chat.newConversation() }) { Text("New chat") }
                                } else if (chat.noModel) {
                                    Spacer(Modifier.weight(1f))
                                    TextButton(shape = RectangleShape, onClick = onOpenModels) { Text("Models") }
                                } else {
                                    Spacer(Modifier.weight(1f))
                                    if (chat.canRetry) {
                                        TextButton(shape = RectangleShape, onClick = { Chat.retry() }) { Text("Send again") }
                                    }
                                    TextButton(shape = RectangleShape, onClick = { Chat.clearError() }) { Text("Dismiss") }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (!followTail && !atBottom && total > 0) {
            val scope = rememberCoroutineScope()
            FilledTonalButton(shape = RectangleShape, 
                onClick = { scope.launch { listState.animateScrollToItem(total - 1, Int.MAX_VALUE) } },
                modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 12.dp),
            ) { Text("Jump to latest") }
        }
    }
}

private fun <T> androidx.compose.foundation.lazy.LazyListScope.itemsIndexed(
    items: List<T>,
    content: @Composable (Int, T) -> Unit,
) = items(items.size) { i -> content(i, items[i]) }

/**
 * One message: a role label, the thinking span folded above an assistant's answer, the answer as
 * Markdown (the user's own text verbatim), the turn's figures, and what can be done with it.
 */
@Composable
private fun MessageView(m: ChatMessage, isLast: Boolean, streaming: Boolean, reasoningOpen: Boolean = false) {
    val isUser = m.role == "user"
    val clipboard = LocalClipboardManager.current
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text(
            if (isUser) "You" else "Assistant",
            fontSize = 12.sp, fontWeight = FontWeight.Bold,
            color = if (isUser) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.tertiary,
        )
        m.choices?.let { choices ->
            ChoiceView(choices, m.best, m.metrics)
            return@Column
        }
        if (m.reasoning.isNotEmpty()) ReasoningBlock(m.reasoning, reasoningOpen)
        SelectionContainer {
            if (isUser) Text(m.content, fontSize = 15.sp) else MarkdownText(m.content)
        }
        if (m.metrics.isNotEmpty()) {
            Text(m.metrics, style = FigureStyle, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        if (!isUser && !streaming) {
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                TextButton(shape = RectangleShape, 
                    onClick = { clipboard.setText(AnnotatedString(m.content)) },
                    contentPadding = PaddingValues(horizontal = 8.dp, vertical = 0.dp),
                ) { Text("Copy", fontSize = 12.sp) }
                if (isLast) {
                    TextButton(shape = RectangleShape, 
                        onClick = { Chat.regenerate() },
                        contentPadding = PaddingValues(horizontal = 8.dp, vertical = 0.dp),
                    ) { Text("Regenerate", fontSize = 12.sp) }
                }
            }
        }
    }
}

/**
 * The model's side of a decision: every option with the probability the model put on it, the
 * picked one in bold. The bars need not add up to 100%: the rest is what the model would have said
 * instead of any option, and a large rest is the model saying it is unsure.
 */
@Composable
private fun ChoiceView(choices: List<io.bigmoeonedge.app.engine.ChoiceScore>, best: Int, metrics: String) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        choices.forEachIndexed { i, c ->
            val picked = i == best
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    "${c.label}) ${c.text}", fontSize = 14.sp,
                    fontWeight = if (picked) FontWeight.Bold else FontWeight.Normal,
                    maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f),
                )
                LinearProgressIndicator(progress = { c.prob.toFloat().coerceIn(0f, 1f) }, modifier = Modifier.width(72.dp).height(6.dp))
                Text(String.format(Locale.US, "%.0f%%", 100.0 * c.prob), fontSize = 12.sp, modifier = Modifier.width(40.dp))
            }
        }
        if (metrics.isNotEmpty()) {
            Text(metrics, fontFamily = FontFamily.Monospace, fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/** The model's reasoning, dimmed and folded: a thinking model spends its first tokens here. */
@Composable
private fun ReasoningBlock(reasoning: String, initiallyExpanded: Boolean) {
    var expanded by rememberSaveable { mutableStateOf(initiallyExpanded) }
    Surface(color = MaterialTheme.colorScheme.surfaceVariant, shape = MaterialTheme.shapes.small, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(horizontal = 10.dp, vertical = 6.dp)) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                modifier = Modifier.fillMaxWidth().clickable { expanded = !expanded },
            ) {
                Text("Thinking", fontSize = 12.sp, fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(if (expanded) "▾" else "▸", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (expanded) {
                SelectionContainer {
                    Text(reasoning, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(top = 4.dp))
                }
            }
        }
    }
}

/** The prompt, where a thumb reaches it, with the live rate above it while an answer streams. */
@Composable
private fun PromptBar() {
    val chat by Chat.state.collectAsStateWithLifecycle()
    val engine by Engine.state.collectAsStateWithLifecycle()
    val focus = LocalFocusManager.current
    var prompt by rememberSaveable { mutableStateOf("") }
    var choose by rememberSaveable { mutableStateOf(false) }
    var optionsText by rememberSaveable { mutableStateOf("") }

    Column(Modifier.fillMaxWidth().imePadding()) {
        LiveLine()
        HorizontalDivider()
        Row(Modifier.padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("Choose from options", fontSize = 12.sp, modifier = Modifier.weight(1f))
            Switch(checked = choose, onCheckedChange = { choose = it }, enabled = !chat.streaming)
        }
        if (choose) {
            OutlinedTextField(
                value = optionsText, onValueChange = { optionsText = it },
                placeholder = { Text("Options, one per line") },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp), minLines = 2, maxLines = 6,
            )
        }
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.Bottom,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedTextField(
                value = prompt,
                onValueChange = { prompt = it },
                placeholder = { Text(if (choose) "Question" else "Message") },
                modifier = Modifier.weight(1f),
                maxLines = 6,
                trailingIcon = if (prompt.isNotEmpty()) ({
                    IconButton(onClick = { prompt = "" }) { Icon(Icons.Filled.Clear, contentDescription = "Clear") }
                }) else null,
            )
            if (chat.streaming) {
                FilledTonalButton(shape = RectangleShape, onClick = { Chat.stop() }, contentPadding = PaddingValues(horizontal = 16.dp)) { Text("Stop") }
            } else {
                val options = if (choose) Decide.parseOptions(optionsText) else emptyList()
                FilledIconButton(shape = RectangleShape, 
                    onClick = {
                        // Drop focus so the keyboard retracts: the answer streams into the space it covered.
                        focus.clearFocus()
                        if (choose) Chat.choose(prompt, options) else Chat.send(prompt)
                        prompt = ""
                    },
                    enabled = prompt.isNotBlank() && engine.serverUp && (!choose || options.size >= 2),
                ) { Icon(Icons.AutoMirrored.Filled.Send, contentDescription = if (choose) "Choose" else "Send") }
            }
        }
    }
}

/** The rate and where the time goes, one line, while generating and after; the full card is in Metrics. */
@Composable
private fun LiveLine() {
    val engine by Engine.state.collectAsStateWithLifecycle()
    val chat by Chat.state.collectAsStateWithLifecycle()
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
    if (chat.streaming && t.steps > 0) parts += "${t.step}/${t.steps}"
    Text(
        parts.joinToString(" · "),
        style = FigureStyle, fontSize = 12.sp,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = 16.dp, vertical = 2.dp),
        maxLines = 1, overflow = TextOverflow.Ellipsis,
    )
}
