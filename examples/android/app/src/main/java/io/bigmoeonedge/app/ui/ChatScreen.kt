package io.bigmoeonedge.app.ui

import androidx.compose.foundation.clickable
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
import androidx.compose.material.icons.filled.DateRange
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Star
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
import io.bigmoeonedge.app.agent.Agent
import io.bigmoeonedge.app.agent.AgentMode
import io.bigmoeonedge.app.agent.Block
import io.bigmoeonedge.app.agent.ConversationSummary
import io.bigmoeonedge.app.agent.RunSummary
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

/** Where the chat's drawer and menus lead, beside the five screens. */
data class AgentNav(
    val openModels: () -> Unit,
    val openSetting: (String) -> Unit,
    val openWorkspaces: () -> Unit,
    val openSkills: () -> Unit,
    val openTasks: () -> Unit,
)

/**
 * The chat: the agent's conversations (the server's, docs/agent.md) in a drawer with the way to
 * workspaces, skills and tasks; the open one in the middle as its items; the composer at the
 * bottom where a thumb reaches it, with what the agent may do, which model answers and in which
 * folder.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChatScreen(modifier: Modifier = Modifier, nav: AgentNav) {
    val agent by Agent.state.collectAsStateWithLifecycle()
    val drawer = rememberDrawerState(DrawerValue.Closed)
    val scope = rememberCoroutineScope()
    var confirmDelete by remember { mutableStateOf<ConversationSummary?>(null) }
    var renaming by remember { mutableStateOf<ConversationSummary?>(null) }
    LaunchedEffect(Unit) { Agent.refreshConversations() }

    ModalNavigationDrawer(
        drawerState = drawer,
        drawerContent = {
            ModalDrawerSheet(drawerContainerColor = tokens.surface) {
                Row(
                    Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 16.dp, bottom = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(Modifier.weight(1f)) { BrandMark() }
                    FilledTonalButton(onClick = { Agent.newConversation(); scope.launch { drawer.close() } }, shape = Corner) {
                        Icon(Icons.Filled.Add, contentDescription = null, Modifier.size(18.dp))
                        Spacer(Modifier.width(6.dp))
                        Text("New chat")
                    }
                }
                val close = { scope.launch { drawer.close() } }
                NavigationDrawerItem(
                    label = { Text("Workspaces") }, icon = { Icon(Icons.Filled.Home, null) }, selected = false,
                    onClick = { close(); nav.openWorkspaces() }, modifier = Modifier.padding(horizontal = 12.dp),
                )
                NavigationDrawerItem(
                    label = { Text("Skills") }, icon = { Icon(Icons.Filled.Star, null) }, selected = false,
                    onClick = { close(); nav.openSkills() }, modifier = Modifier.padding(horizontal = 12.dp),
                )
                NavigationDrawerItem(
                    label = { Text("Tasks") }, icon = { Icon(Icons.Filled.DateRange, null) }, selected = false,
                    onClick = { close(); nav.openTasks() }, modifier = Modifier.padding(horizontal = 12.dp),
                )
                HorizontalDivider(Modifier.padding(vertical = 8.dp), color = tokens.line)
                SectionTitle("Conversations", Modifier.padding(horizontal = 28.dp, vertical = 4.dp))
                val stamp = remember { SimpleDateFormat("d MMM HH:mm", Locale.getDefault()) }
                if (agent.conversations.isEmpty()) {
                    Text(
                        "None yet. What you send starts one, kept by the engine on this phone.",
                        style = MaterialTheme.typography.bodySmall, color = tokens.muted,
                        modifier = Modifier.padding(horizontal = 28.dp, vertical = 8.dp),
                    )
                }
                LazyColumn(contentPadding = PaddingValues(horizontal = 12.dp, vertical = 4.dp)) {
                    items(agent.conversations, key = { it.id }) { c ->
                        var menu by remember { mutableStateOf(false) }
                        NavigationDrawerItem(
                            selected = c.id == agent.open?.id,
                            onClick = { Agent.open(c.id); scope.launch { drawer.close() } },
                            label = {
                                Column {
                                    Text(c.title.ifEmpty { "New chat" }, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                        val meta = listOfNotNull(
                                            c.updated.takeIf { it > 0 }?.let { stamp.format(Date(it)) },
                                            agent.workspaces.firstOrNull { it.id == c.workspace }?.name,
                                            if (c.task.isNotEmpty()) "task" else null,
                                        ).joinToString(" · ")
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
                    Column {
                        Text(agent.open?.title?.ifEmpty { null } ?: "New chat", maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.titleMedium)
                        agent.workspace?.let { Text(it.name, style = MaterialTheme.typography.labelSmall, color = tokens.muted, maxLines = 1) }
                    }
                },
                navigationIcon = {
                    IconButton(onClick = { scope.launch { drawer.open() } }) { Icon(Icons.Filled.Menu, contentDescription = "Conversations") }
                },
                actions = {
                    if (agent.open != null) {
                        IconButton(onClick = { Agent.newConversation() }) { Icon(Icons.Filled.Add, contentDescription = "New chat") }
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = tokens.bg),
            )
            HorizontalDivider(color = tokens.line)
            Transcript(Modifier.weight(1f), nav)
            Composer(nav)
        }
    }

    confirmDelete?.let { c ->
        AlertDialog(
            onDismissRequest = { confirmDelete = null },
            title = { Text("Delete this conversation?") },
            text = { Text("\"${c.title.ifEmpty { "New chat" }}\" and everything in it is removed from the engine. Files the agent wrote stay in their folder.") },
            confirmButton = { TextButton(onClick = { Agent.delete(c.id); confirmDelete = null }) { Text("Delete", color = tokens.err) } },
            dismissButton = { TextButton(onClick = { confirmDelete = null }) { Text("Cancel") } },
        )
    }
    renaming?.let { c ->
        var title by remember(c.id) { mutableStateOf(c.title) }
        AlertDialog(
            onDismissRequest = { renaming = null },
            title = { Text("Rename") },
            text = { OutlinedTextField(value = title, onValueChange = { title = it }, singleLine = true, modifier = Modifier.fillMaxWidth()) },
            confirmButton = { TextButton(onClick = { Agent.rename(c.id, title.trim()); renaming = null }, enabled = title.isNotBlank()) { Text("Save") } },
            dismissButton = { TextButton(onClick = { renaming = null }) { Text("Cancel") } },
        )
    }
}

@Composable
fun ConversationStateChip(state: String) {
    when (state) {
        ConversationSummary.STATE_RUNNING -> Chip("running", tokens.accentInk)
        ConversationSummary.STATE_WAITING -> Chip("needs you", tokens.warn)
        ConversationSummary.STATE_ERROR -> Chip("error", tokens.err)
    }
}

@Composable
private fun Transcript(modifier: Modifier, nav: AgentNav) {
    val agent by Agent.state.collectAsStateWithLifecycle()
    val decision by Decisions.state.collectAsStateWithLifecycle()
    // Only the context advice, not every token event: the transcript is the heaviest thing on screen.
    val ctxAdvice by remember { Engine.state.map { contextWarning(it.info?.model, it.params) }.distinctUntilChanged() }.collectAsStateWithLifecycle(null)
    val listState = rememberLazyListState()
    val blocks = remember(agent.items) { RunSummary.blocks(agent.items) }
    val working = agent.running && agent.items.lastOrNull()?.let { !it.streaming && !it.awaitsApproval } != false
    val total = blocks.size + (if (working || agent.sending) 1 else 0) + (if (decision != null) 1 else 0) + (if (agent.error != null) 1 else 0)
    var preview by remember { mutableStateOf<String?>(null) }
    val lastText = agent.items.lastOrNull()?.let { it.text.length + it.reasoning.length } ?: 0

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
        if (agent.opening) {
            LinearProgressIndicator(Modifier.fillMaxWidth().align(Alignment.TopCenter))
        }
        if (blocks.isEmpty() && decision == null && agent.error == null && !agent.sending && !agent.opening) {
            EmptyChat(agent.unsupported)
        }
        LazyColumn(
            state = listState,
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            items(blocks.size) { i ->
                when (val b = blocks[i]) {
                    is Block.One -> ItemView(b.item, isLast = i == blocks.lastIndex, running = agent.running, onPreview = { preview = it })
                    is Block.Run -> RunGroup(b, onPreview = { preview = it })
                }
            }
            if (working || agent.sending) {
                item(key = "working") {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp, color = tokens.accent)
                        Text(if (agent.sending) "Sending…" else "Working…", style = MaterialTheme.typography.bodySmall, color = tokens.muted)
                    }
                }
            }
            decision?.let { d -> item(key = "decision") { DecisionCard(d, nav.openModels) } }
            agent.error?.let { err ->
                item(key = "error") {
                    ErrorCard(err, agent.errorCode, ctxAdvice?.let { it.nCtx to (it.key to it.label) }, nav)
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
    preview?.let { path -> FilePreviewSheet(path, workspacePath = agent.workspace?.path, onDismiss = { preview = null }) }
}

@Composable
private fun EmptyChat(unsupported: Boolean) {
    Column(Modifier.fillMaxSize().padding(24.dp), verticalArrangement = Arrangement.Center) {
        if (unsupported) {
            Text("This engine has no agent.", style = MaterialTheme.typography.titleMedium)
            Text(
                "The server this app started does not offer conversations. Update the app to a build that carries them.",
                style = MaterialTheme.typography.bodyMedium, color = tokens.muted,
            )
            return@Column
        }
        Text("Ask anything.", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(4.dp))
        Text(
            "The model runs on this phone. Pick a folder below and the agent can read and change the " +
                "files in it, asking first unless you allow more. Load a model from Models if none is loaded.",
            style = MaterialTheme.typography.bodyMedium, color = tokens.muted,
        )
    }
}

/** Why a send or a run failed, decided on by its code; the fix sits beside it. */
@Composable
private fun ErrorCard(err: String, code: String, ctx: Pair<Int, Pair<String, String>>?, nav: AgentNav) {
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
                    TextButton(onClick = { Agent.newConversation() }) { Text("New chat") }
                }
                code == ApiException.CODE_NO_MODEL -> TextButton(onClick = nav.openModels) { Text("Models") }
            }
            TextButton(onClick = { Agent.clearError() }) { Text("Dismiss") }
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
 * The composer: the live line while the engine works, the three things a conversation runs with
 * (mode, model, folder) as chips, then the prompt and Send, or Stop while a run goes on.
 */
@Composable
private fun Composer(nav: AgentNav) {
    val agent by Agent.state.collectAsStateWithLifecycle()
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
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 12.dp).padding(top = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                ModeChip(agent.mode, enabled = !agent.running)
                RouteChip(agent, enabled = !agent.running)
                WorkspaceChip(agent, enabled = !agent.running, onManage = nav.openWorkspaces)
            }
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
                if (agent.running) {
                    FilledTonalButton(onClick = { Agent.stop() }, shape = Corner, contentPadding = PaddingValues(horizontal = 16.dp)) { Text("Stop") }
                } else {
                    val options = if (choose) Decide.parseOptions(optionsText) else emptyList()
                    FilledIconButton(
                        shape = Corner,
                        onClick = {
                            // Drop focus so the keyboard retracts: the answer streams into the space it covered.
                            focus.clearFocus()
                            if (choose) Decisions.choose(prompt, options) else Agent.send(prompt)
                            prompt = ""
                        },
                        enabled = prompt.isNotBlank() && engine.serverUp && !agent.sending && decision?.running != true &&
                            (!choose || options.size >= 2),
                    ) { Icon(Icons.AutoMirrored.Filled.Send, contentDescription = if (choose) "Choose" else "Send") }
                }
            }
        }
    }
}

/** A chip with a menu under it: the composer's three choices look and behave alike. */
@Composable
private fun MenuChip(label: String, icon: androidx.compose.ui.graphics.vector.ImageVector?, enabled: Boolean, modifier: Modifier = Modifier, menu: @Composable ColumnScope.(close: () -> Unit) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box(modifier) {
        AssistChip(
            onClick = { open = true },
            enabled = enabled,
            label = { Text(label, maxLines = 1, overflow = TextOverflow.Ellipsis) },
            leadingIcon = icon?.let { { Icon(it, null, Modifier.size(16.dp)) } },
            shape = Corner,
            colors = AssistChipDefaults.assistChipColors(containerColor = tokens.surface2, labelColor = tokens.text, leadingIconContentColor = tokens.muted),
            border = null,
        )
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) { menu { open = false } }
    }
}

@Composable
private fun ModeChip(mode: AgentMode, enabled: Boolean) {
    MenuChip(mode.label, Icons.Filled.Lock, enabled) { close ->
        AgentMode.values().forEach { m ->
            DropdownMenuItem(
                text = {
                    Column {
                        Text(m.label, fontWeight = if (m == mode) FontWeight.SemiBold else FontWeight.Normal)
                        Text(m.help, style = MaterialTheme.typography.bodySmall, color = tokens.muted)
                    }
                },
                onClick = { Agent.setMode(m); close() },
            )
        }
    }
}

@Composable
private fun RouteChip(agent: io.bigmoeonedge.app.agent.AgentUi, enabled: Boolean) {
    val routes = agent.routes
    val current = routes.firstOrNull { it.wire == agent.route }?.label ?: agent.route
    MenuChip(current, null, enabled, Modifier.widthIn(max = 160.dp)) { close ->
        routes.forEach { r ->
            DropdownMenuItem(
                text = { Text(r.label, fontWeight = if (r.wire == agent.route) FontWeight.SemiBold else FontWeight.Normal) },
                onClick = { Agent.setRoute(r.wire); close() },
            )
        }
        if (routes.size == 1) {
            Text(
                "Add a provider in Settings to answer with a model on another service.",
                style = MaterialTheme.typography.bodySmall, color = tokens.muted,
                modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp).widthIn(max = 240.dp),
            )
        }
    }
}

@Composable
private fun WorkspaceChip(agent: io.bigmoeonedge.app.agent.AgentUi, enabled: Boolean, onManage: () -> Unit) {
    val ws = agent.workspace
    MenuChip(ws?.name ?: "No folder", Icons.Filled.Home, enabled) { close ->
        DropdownMenuItem(
            text = { Text("No folder", fontWeight = if (ws == null) FontWeight.SemiBold else FontWeight.Normal) },
            onClick = { Agent.setWorkspace(""); close() },
        )
        agent.workspaces.forEach { w ->
            DropdownMenuItem(
                text = { Text(w.name.ifEmpty { w.path }, fontWeight = if (w.id == ws?.id) FontWeight.SemiBold else FontWeight.Normal) },
                onClick = { Agent.setWorkspace(w.id); close() },
            )
        }
        HorizontalDivider(color = tokens.line)
        DropdownMenuItem(text = { Text("Manage folders…") }, onClick = { close(); onManage() })
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
