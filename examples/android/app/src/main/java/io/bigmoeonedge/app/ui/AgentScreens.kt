package io.bigmoeonedge.app.ui

import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.agent.Agent
import io.bigmoeonedge.app.agent.AgentMode
import io.bigmoeonedge.app.agent.AgentTask
import io.bigmoeonedge.app.agent.LocalFolders
import io.bigmoeonedge.app.agent.Provider
import io.bigmoeonedge.app.agent.Schedule
import io.bigmoeonedge.app.agent.Schedules
import io.bigmoeonedge.app.agent.Skill
import io.bigmoeonedge.app.agent.Workspace
import io.bigmoeonedge.app.agent.routesOf
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** A screen the chat's drawer or Settings opens over the five: a bar with Back, then its content. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SubScreen(title: String, onBack: () -> Unit, modifier: Modifier = Modifier, actions: @Composable RowScope.() -> Unit = {}, content: @Composable ColumnScope.() -> Unit) {
    Column(modifier.fillMaxSize()) {
        TopAppBar(
            title = { Text(title) },
            navigationIcon = { IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") } },
            actions = actions,
            colors = TopAppBarDefaults.topAppBarColors(containerColor = tokens.bg),
        )
        HorizontalDivider(color = tokens.line)
        content()
    }
}

@Composable
private fun Toasting(): (Result<*>, String?) -> Unit {
    val ctx = LocalContext.current
    return { r, ok -> r.fold({ ok?.let { Toast.makeText(ctx, it, Toast.LENGTH_SHORT).show() } }, { Toast.makeText(ctx, it.message ?: "Failed", Toast.LENGTH_LONG).show() }) }
}

// ── workspaces ──

/**
 * The folders the agent can work in. On a phone a workspace is a folder in this app's own storage:
 * made and named here, files brought in through the system picker and sent out through the share
 * sheet. A folder the server lists that is not one of the app's (another client added it) is
 * listed, and can be forgotten, but its files are not browsed from here.
 */
@Composable
fun WorkspacesScreen(modifier: Modifier, onBack: () -> Unit) {
    val agent by Agent.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val report = Toasting()
    var creating by remember { mutableStateOf(false) }
    var browsing by remember { mutableStateOf<Workspace?>(null) }
    LaunchedEffect(Unit) { Agent.refreshWorkspaces() }

    browsing?.let { w ->
        FolderScreen(w, modifier, onBack = { browsing = null })
        return
    }
    SubScreen("Workspaces", onBack, modifier, actions = {
        IconButton(onClick = { creating = true }) { Icon(Icons.Filled.Add, contentDescription = "New folder") }
    }) {
        LazyColumn(contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            item {
                Text(
                    "A workspace is a folder the agent can read and, in the mode you pick, change. Folders made here " +
                        "live in this app's storage: bring files in with Import, send one out with Share.",
                    style = MaterialTheme.typography.bodyMedium, color = tokens.muted,
                )
            }
            if (agent.workspaces.isEmpty()) {
                item { Button(onClick = { creating = true }, shape = Corner) { Text("New folder") } }
            }
            items(agent.workspaces, key = { it.id }) { w ->
                var confirm by remember { mutableStateOf(false) }
                BmoeCard(Modifier.clickable { browsing = w }, padding = 12.dp) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(w.name.ifEmpty { File(w.path).name }, style = MaterialTheme.typography.titleSmall)
                            Text(w.path, style = CodeStyle, color = tokens.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        }
                        IconButton(onClick = { confirm = true }) { Icon(Icons.Filled.Delete, contentDescription = "Forget") }
                    }
                }
                if (confirm) {
                    AlertDialog(
                        onDismissRequest = { confirm = false },
                        title = { Text("Forget this folder?") },
                        text = { Text("The agent stops working in it. Its files stay where they are.") },
                        confirmButton = { TextButton(onClick = { confirm = false; scope.launch { report(Agent.forgetWorkspace(w.id), null) } }) { Text("Forget") } },
                        dismissButton = { TextButton(onClick = { confirm = false }) { Text("Cancel") } },
                    )
                }
            }
        }
    }
    if (creating) {
        var name by remember { mutableStateOf("") }
        AlertDialog(
            onDismissRequest = { creating = false },
            title = { Text("New folder") },
            text = { OutlinedTextField(value = name, onValueChange = { name = it }, label = { Text("Name") }, singleLine = true) },
            confirmButton = {
                TextButton(
                    onClick = { creating = false; scope.launch { report(Agent.createWorkspace(name), "Folder ready") } },
                    enabled = LocalFolders.safeName(name).isNotEmpty(),
                ) { Text("Create") }
            },
            dismissButton = { TextButton(onClick = { creating = false }) { Text("Cancel") } },
        )
    }
}

/** One of the app's folders: what is in it, files brought in, a file shared out. */
@Composable
private fun FolderScreen(w: Workspace, modifier: Modifier, onBack: () -> Unit) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val report = Toasting()
    val root = remember(w.path) { File(w.path) }
    var dir by remember(w.path) { mutableStateOf(root) }
    var entries by remember { mutableStateOf<List<File>>(emptyList()) }
    var tick by remember { mutableStateOf(0) }
    val local = remember(w.path) { LocalFolders.isLocal(ctx, w.path) }
    LaunchedEffect(dir, tick) { entries = withContext(Dispatchers.IO) { LocalFolders.list(dir) } }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isEmpty()) return@rememberLauncherForActivityResult
        scope.launch {
            val r = withContext(Dispatchers.IO) { runCatching { uris.forEach { LocalFolders.importInto(ctx, dir, it) } } }
            report(r, "Imported ${uris.size} file" + if (uris.size == 1) "" else "s")
            tick++
        }
    }
    val back = { if (dir != root) dir = dir.parentFile ?: root else onBack() }
    SubScreen(w.name.ifEmpty { root.name }, onBack = back, modifier = modifier, actions = {
        if (local) TextButton(onClick = { picker.launch(arrayOf("*/*")) }) { Text("Import") }
    }) {
        if (!local) {
            Text(
                "This folder is not in this app's storage, so its files are not shown here.",
                style = MaterialTheme.typography.bodyMedium, color = tokens.muted, modifier = Modifier.padding(16.dp),
            )
            return@SubScreen
        }
        if (dir != root) Text(dir.relativeTo(root).path, style = CodeStyle, color = tokens.muted, modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
        if (entries.isEmpty()) {
            Text("Empty. Import files, or ask the agent to write some.", style = MaterialTheme.typography.bodyMedium, color = tokens.muted, modifier = Modifier.padding(16.dp))
        }
        LazyColumn {
            items(entries, key = { it.path }) { f ->
                ListItem(
                    headlineContent = { Text(f.name + if (f.isDirectory) "/" else "", maxLines = 1, overflow = TextOverflow.Ellipsis) },
                    supportingContent = if (f.isFile) ({ Text(bytesShort(f.length()), style = FigureStyle, color = tokens.muted) }) else null,
                    trailingContent = if (f.isFile) ({
                        IconButton(onClick = { ctx.startActivity(LocalFolders.shareIntent(ctx, f)) }) { Icon(Icons.Filled.Share, contentDescription = "Share") }
                    }) else null,
                    modifier = Modifier.clickable(enabled = f.isDirectory) { dir = f },
                    colors = ListItemDefaults.colors(containerColor = tokens.bg),
                )
                HorizontalDivider(color = tokens.line)
            }
        }
    }
}

private fun bytesShort(b: Long): String = when {
    b >= 1L shl 30 -> String.format(Locale.US, "%.1f GB", b / (1L shl 30).toDouble())
    b >= 1L shl 20 -> String.format(Locale.US, "%.1f MB", b / (1L shl 20).toDouble())
    b >= 1L shl 10 -> String.format(Locale.US, "%.0f KB", b / (1L shl 10).toDouble())
    else -> "$b bytes"
}

// ── skills ──

/** The skills the agent can follow: switched on and off, and a person's own written here. */
@Composable
fun SkillsScreen(modifier: Modifier, onBack: () -> Unit) {
    val agent by Agent.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val report = Toasting()
    var editing by remember { mutableStateOf<Skill?>(null) }
    var isNew by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { Agent.refreshSkills() }

    editing?.let { s ->
        SkillEditor(s, isNew, modifier, onBack = { editing = null }, onSaved = { editing = null })
        return
    }
    SubScreen("Skills", onBack, modifier, actions = {
        IconButton(onClick = { isNew = true; editing = Skill("", "", "", Skill.SOURCE_USER, true, "") }) { Icon(Icons.Filled.Add, contentDescription = "New skill") }
    }) {
        val skills = agent.skills
        LazyColumn(contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            item {
                Text(
                    "A skill is a set of instructions the agent reads when a job calls for it. It sees the name and " +
                        "description of every skill that is on.",
                    style = MaterialTheme.typography.bodyMedium, color = tokens.muted,
                )
            }
            if (skills == null) item { LinearProgressIndicator(Modifier.fillMaxWidth()) }
            else if (skills.isEmpty()) item { Text("No skills yet.", color = tokens.muted) }
            items(skills.orEmpty(), key = { it.source + "/" + it.name }) { s ->
                BmoeCard(Modifier.clickable { isNew = false; editing = s }, padding = 12.dp) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                                Text(s.title.ifEmpty { s.name }, style = MaterialTheme.typography.titleSmall)
                                Chip(sourceLabel(s.source), tokens.muted)
                            }
                            Text(s.description, style = MaterialTheme.typography.bodySmall, color = tokens.muted, maxLines = 3, overflow = TextOverflow.Ellipsis)
                        }
                        Switch(checked = s.enabled, onCheckedChange = { on -> scope.launch { report(Agent.enableSkill(s.name, on), null) } })
                    }
                }
            }
        }
    }
}

private fun sourceLabel(source: String) = when (source) {
    "bundled" -> "built in"
    "user" -> "yours"
    "workspace" -> "folder"
    else -> source
}

@Composable
private fun SkillEditor(s: Skill, isNew: Boolean, modifier: Modifier, onBack: () -> Unit, onSaved: () -> Unit) {
    val scope = rememberCoroutineScope()
    val report = Toasting()
    var name by remember { mutableStateOf(s.name) }
    var description by remember { mutableStateOf(s.description) }
    var body by remember { mutableStateOf(s.body) }
    var confirmDelete by remember { mutableStateOf(false) }
    val editable = isNew || s.editable
    SubScreen(if (isNew) "New skill" else s.title.ifEmpty { s.name }, onBack, modifier, actions = {
        if (!isNew && s.editable) IconButton(onClick = { confirmDelete = true }) { Icon(Icons.Filled.Delete, contentDescription = "Delete") }
    }) {
        Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            if (!editable) Text("A ${sourceLabel(s.source)} skill can be switched on and off, not edited.", color = tokens.muted, style = MaterialTheme.typography.bodySmall)
            OutlinedTextField(
                value = name, onValueChange = { name = it.lowercase() }, label = { Text("Name") }, singleLine = true,
                enabled = isNew, isError = name.isNotEmpty() && !Skill.validName(name),
                supportingText = { Text("Lower-case words joined by dashes, like write-release-notes") },
                textStyle = CodeStyle, modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = description, onValueChange = { description = it }, label = { Text("Description") }, enabled = editable,
                supportingText = { Text("When the agent should use it: it reads this to decide") }, minLines = 2, modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = body, onValueChange = { body = it }, label = { Text("Instructions") }, enabled = editable,
                minLines = 8, textStyle = CodeStyle, modifier = Modifier.fillMaxWidth(),
            )
            if (editable) {
                Button(
                    onClick = { scope.launch { val r = Agent.saveSkill(name, description.trim(), body, isNew); report(r, "Saved"); if (r.isSuccess) onSaved() } },
                    enabled = Skill.validName(name) && description.isNotBlank() && body.isNotBlank(),
                    shape = Corner, modifier = Modifier.align(Alignment.End),
                ) { Text("Save") }
            }
        }
    }
    if (confirmDelete) {
        AlertDialog(
            onDismissRequest = { confirmDelete = false },
            title = { Text("Delete this skill?") },
            confirmButton = { TextButton(onClick = { confirmDelete = false; scope.launch { val r = Agent.deleteSkill(s.name); report(r, null); if (r.isSuccess) onSaved() } }) { Text("Delete", color = tokens.err) } },
            dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text("Cancel") } },
        )
    }
}

// ── tasks ──

/** Prompts that run on a schedule, each as a conversation of its own. */
@Composable
fun TasksScreen(modifier: Modifier, onBack: () -> Unit, onOpenConversation: (String) -> Unit) {
    val agent by Agent.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val report = Toasting()
    var editing by remember { mutableStateOf<AgentTask?>(null) }
    LaunchedEffect(Unit) { Agent.refreshTasks() }
    val stamp = remember { SimpleDateFormat("d MMM HH:mm", Locale.getDefault()) }

    editing?.let { t ->
        TaskEditor(t, modifier, onBack = { editing = null })
        return
    }
    SubScreen("Tasks", onBack, modifier, actions = {
        IconButton(onClick = { editing = AgentTask.blank() }) { Icon(Icons.Filled.Add, contentDescription = "New task") }
    }) {
        val tasks = agent.tasks
        LazyColumn(contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            item {
                BmoeCard(colour = tokens.surface2, padding = 12.dp) {
                    Text(
                        "Tasks run only while this app's engine is running. The engine keeps going in the background " +
                            "while its notification is shown; if you stop it, or the phone ends it, a task that came due " +
                            "meanwhile runs when the engine is back.",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
            if (tasks == null) item { LinearProgressIndicator(Modifier.fillMaxWidth()) }
            else if (tasks.isEmpty()) item { Text("No tasks yet. A task is a prompt that runs on its own, once or on a schedule.", color = tokens.muted) }
            items(tasks.orEmpty(), key = { it.id }) { t ->
                BmoeCard(Modifier.clickable { editing = t }, padding = 12.dp) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text(t.title.ifEmpty { t.prompt.take(40) }, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Text(Schedules.describe(t.schedule) + " · " + t.mode.label, style = MaterialTheme.typography.bodySmall, color = tokens.muted)
                            if (t.nextRun > 0 && t.enabled) Text("Next: " + stamp.format(Date(t.nextRun)), style = FigureStyle, color = tokens.muted)
                            t.lastRun?.takeIf { it.at > 0 }?.let { lr ->
                                Text(
                                    "Last: " + stamp.format(Date(lr.at)) + (if (lr.outcome.isNotEmpty()) " · " + lr.outcome else ""),
                                    style = FigureStyle, color = tokens.muted,
                                    modifier = Modifier.clickable(enabled = lr.conversation.isNotEmpty()) { onOpenConversation(lr.conversation) },
                                )
                            }
                        }
                        Switch(checked = t.enabled, onCheckedChange = { on -> scope.launch { report(Agent.saveTask(t.copy(enabled = on)), null) } })
                    }
                    Row {
                        Spacer(Modifier.weight(1f))
                        TextButton(onClick = { scope.launch { report(Agent.runTask(t.id), "Started") } }) { Text("Run now") }
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TaskEditor(t: AgentTask, modifier: Modifier, onBack: () -> Unit) {
    val agent by Agent.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val report = Toasting()
    var title by remember { mutableStateOf(t.title) }
    var prompt by remember { mutableStateOf(t.prompt) }
    var kind by remember { mutableStateOf(t.schedule.kind) }
    var at by remember { mutableStateOf(t.schedule.at.ifEmpty { if (t.schedule.kind == Schedule.ONCE) "" else "09:00" }) }
    var every by remember { mutableStateOf(t.schedule.everyMinutes.takeIf { it > 0 }?.toString() ?: "60") }
    var days by remember { mutableStateOf(t.schedule.weekdays.toSet().ifEmpty { setOf(1) }) }
    var mode by remember { mutableStateOf(t.mode) }
    var route by remember { mutableStateOf(t.route) }
    var workspace by remember { mutableStateOf(t.workspace) }
    var confirmDelete by remember { mutableStateOf(false) }
    val schedule = Schedule(kind, if (kind == Schedule.INTERVAL) "" else at.trim(), every.toIntOrNull() ?: 0, if (kind == Schedule.WEEKLY) days.sorted() else emptyList())
    val valid = prompt.isNotBlank() && when (kind) {
        Schedule.ONCE -> Regex("""\d{4}-\d{2}-\d{2}T\d{2}:\d{2}""").matches(at.trim())
        Schedule.INTERVAL -> (every.toIntOrNull() ?: 0) > 0
        Schedule.DAILY -> Regex("""\d{2}:\d{2}""").matches(at.trim())
        else -> Regex("""\d{2}:\d{2}""").matches(at.trim()) && days.isNotEmpty()
    }

    SubScreen(if (t.id.isEmpty()) "New task" else t.title.ifEmpty { "Task" }, onBack, modifier, actions = {
        if (t.id.isNotEmpty()) IconButton(onClick = { confirmDelete = true }) { Icon(Icons.Filled.Delete, contentDescription = "Delete") }
    }) {
        Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            OutlinedTextField(value = title, onValueChange = { title = it }, label = { Text("Title") }, singleLine = true, modifier = Modifier.fillMaxWidth())
            OutlinedTextField(value = prompt, onValueChange = { prompt = it }, label = { Text("Prompt") }, minLines = 4, modifier = Modifier.fillMaxWidth())
            SectionTitle("When")
            SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                val labels = mapOf(Schedule.ONCE to "Once", Schedule.INTERVAL to "Repeat", Schedule.DAILY to "Daily", Schedule.WEEKLY to "Weekly")
                Schedule.KINDS.forEachIndexed { i, k ->
                    SegmentedButton(
                        selected = kind == k,
                        onClick = {
                            kind = k
                            if (k == Schedule.ONCE && !at.contains('T')) at = java.time.LocalDate.now().plusDays(1).toString() + "T" + at.ifEmpty { "09:00" }
                            if (k != Schedule.ONCE && at.contains('T')) at = at.substringAfter('T')
                        },
                        shape = SegmentedButtonDefaults.itemShape(i, Schedule.KINDS.size),
                    ) { Text(labels[k] ?: k) }
                }
            }
            when (kind) {
                Schedule.ONCE -> OutlinedTextField(
                    value = at, onValueChange = { at = it }, label = { Text("Date and time") }, singleLine = true,
                    supportingText = { Text("YYYY-MM-DDTHH:MM, this phone's time") }, textStyle = CodeStyle, modifier = Modifier.fillMaxWidth(),
                )
                Schedule.INTERVAL -> OutlinedTextField(
                    value = every, onValueChange = { every = it.filter(Char::isDigit) }, label = { Text("Every (minutes)") }, singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number), modifier = Modifier.fillMaxWidth(),
                )
                else -> {
                    OutlinedTextField(
                        value = at, onValueChange = { at = it }, label = { Text("Time") }, singleLine = true,
                        supportingText = { Text("HH:MM, this phone's time") }, textStyle = CodeStyle, modifier = Modifier.fillMaxWidth(),
                    )
                    if (kind == Schedule.WEEKLY) {
                        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                            listOf(1, 2, 3, 4, 5, 6, 0).forEach { d ->
                                FilterChip(
                                    selected = d in days, onClick = { days = if (d in days) days - d else days + d },
                                    label = { Text(Schedules.dayName(d).take(2)) }, shape = Corner,
                                )
                            }
                        }
                    }
                }
            }
            Text(if (valid) Schedules.describe(schedule) else "Fill in when it runs.", style = MaterialTheme.typography.bodySmall, color = tokens.muted)
            SectionTitle("How")
            LabeledDropdown("Mode", AgentMode.values().map { it.label + ": " + it.help }, AgentMode.values().indexOf(mode)) { mode = AgentMode.values()[it] }
            val routes = routesOf(agent.providers)
            LabeledDropdown("Model", routes.map { it.label }, routes.indexOfFirst { it.wire == route }.coerceAtLeast(0)) { route = routes[it].wire }
            val folders = listOf(Workspace("", "No folder", "")) + agent.workspaces
            LabeledDropdown("Folder", folders.map { it.name.ifEmpty { it.path } }, folders.indexOfFirst { it.id == workspace }.coerceAtLeast(0)) { workspace = folders[it].id }
            Text(
                "Nobody is at the screen when a task runs: anything that would ask for your approval is answered no, and the result says what was not done.",
                style = MaterialTheme.typography.bodySmall, color = tokens.muted,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.align(Alignment.End)) {
                if (t.id.isNotEmpty()) OutlinedButton(onClick = { scope.launch { report(Agent.runTask(t.id), "Started") } }, shape = Corner) { Text("Run now") }
                Button(
                    onClick = {
                        scope.launch {
                            val r = Agent.saveTask(t.copy(title = title.trim(), prompt = prompt, schedule = schedule, mode = mode, route = route, workspace = workspace))
                            report(r, "Saved")
                            if (r.isSuccess) onBack()
                        }
                    },
                    enabled = valid, shape = Corner,
                ) { Text("Save") }
            }
        }
    }
    if (confirmDelete) {
        AlertDialog(
            onDismissRequest = { confirmDelete = false },
            title = { Text("Delete this task?") },
            text = { Text("The conversations its runs left stay.") },
            confirmButton = { TextButton(onClick = { confirmDelete = false; scope.launch { val r = Agent.deleteTask(t.id); report(r, null); if (r.isSuccess) onBack() } }) { Text("Delete", color = tokens.err) } },
            dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text("Cancel") } },
        )
    }
}

// ── providers ──

/**
 * OpenAI-compatible services a conversation can answer from instead of the phone's model. The key
 * is written once and never shown again: the server keeps it and says only that it has one.
 */
@Composable
fun ProvidersScreen(modifier: Modifier, onBack: () -> Unit) {
    val agent by Agent.state.collectAsStateWithLifecycle()
    var editing by remember { mutableStateOf<Provider?>(null) }
    LaunchedEffect(Unit) { Agent.refreshProviders() }
    editing?.let { p ->
        ProviderEditor(p, modifier, onBack = { editing = null })
        return
    }
    SubScreen("Providers", onBack, modifier, actions = {
        IconButton(onClick = { editing = Provider("", "", "", emptyList(), false) }) { Icon(Icons.Filled.Add, contentDescription = "Add provider") }
    }) {
        LazyColumn(contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            item {
                Text(
                    "A provider is a chat service a conversation can use instead of the model on this phone. Only " +
                        "conversations you point at it send their messages there.",
                    style = MaterialTheme.typography.bodyMedium, color = tokens.muted,
                )
            }
            if (agent.providers.isEmpty()) item { Button(onClick = { editing = Provider("", "", "", emptyList(), false) }, shape = Corner) { Text("Add a provider") } }
            items(agent.providers, key = { it.id }) { p ->
                BmoeCard(Modifier.clickable { editing = p }, padding = 12.dp) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(p.name.ifEmpty { p.id }, style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f))
                        if (p.hasKey) Chip("key saved", tokens.ok) else Chip("no key", tokens.muted)
                    }
                    Text(p.baseUrl, style = CodeStyle, color = tokens.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    if (p.models.isNotEmpty()) Text(p.models.joinToString(", "), style = MaterialTheme.typography.bodySmall, color = tokens.muted)
                }
            }
        }
    }
}

@Composable
private fun ProviderEditor(p: Provider, modifier: Modifier, onBack: () -> Unit) {
    val scope = rememberCoroutineScope()
    val report = Toasting()
    var name by remember { mutableStateOf(p.name) }
    var baseUrl by remember { mutableStateOf(p.baseUrl) }
    var models by remember { mutableStateOf(p.models.joinToString(", ")) }
    var key by remember { mutableStateOf("") }
    var confirmDelete by remember { mutableStateOf(false) }
    val modelList = models.split(',', '\n').map { it.trim() }.filter { it.isNotEmpty() }
    SubScreen(if (p.id.isEmpty()) "Add provider" else p.name.ifEmpty { p.id }, onBack, modifier, actions = {
        if (p.id.isNotEmpty()) IconButton(onClick = { confirmDelete = true }) { Icon(Icons.Filled.Delete, contentDescription = "Delete") }
    }) {
        Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            OutlinedTextField(value = name, onValueChange = { name = it }, label = { Text("Name") }, singleLine = true, modifier = Modifier.fillMaxWidth())
            OutlinedTextField(
                value = baseUrl, onValueChange = { baseUrl = it.trim() }, label = { Text("Base URL") }, singleLine = true,
                supportingText = { Text("The service's OpenAI-compatible address, ending in /v1") },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri), textStyle = CodeStyle, modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = models, onValueChange = { models = it }, label = { Text("Models") },
                supportingText = { Text("Comma-separated model names") }, modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = key, onValueChange = { key = it.trim() }, label = { Text("API key") }, singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                supportingText = { Text(if (p.hasKey) "A key is saved. Type a new one to replace it; it is never shown again." else "Saved by the engine on this phone and never shown again.") },
                modifier = Modifier.fillMaxWidth(),
            )
            Button(
                onClick = { scope.launch { val r = Agent.saveProvider(p.id, name.trim(), baseUrl, modelList, key); report(r, "Saved"); if (r.isSuccess) onBack() } },
                enabled = name.isNotBlank() && baseUrl.startsWith("http") && modelList.isNotEmpty(),
                shape = Corner, modifier = Modifier.align(Alignment.End),
            ) { Text("Save") }
        }
    }
    if (confirmDelete) {
        AlertDialog(
            onDismissRequest = { confirmDelete = false },
            title = { Text("Delete this provider?") },
            text = { Text("Its key is deleted with it. Conversations that used it keep their messages.") },
            confirmButton = { TextButton(onClick = { confirmDelete = false; scope.launch { val r = Agent.deleteProvider(p.id); report(r, null); if (r.isSuccess) onBack() } }) { Text("Delete", color = tokens.err) } },
            dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text("Cancel") } },
        )
    }
}

/** Settings' entry to the agent: providers, skills, tasks, folders. */
@Composable
fun AgentSettingsCard(onOpen: (AgentPage) -> Unit) {
    BmoeCard(padding = 12.dp) {
        Text("Agent", style = MaterialTheme.typography.titleMedium, color = tokens.accentInk)
        Text("What conversations can use beside the model on this phone.", style = MaterialTheme.typography.bodySmall, color = tokens.muted)
        AgentPage.values().forEach { p ->
            ListItem(
                headlineContent = { Text(p.title) },
                supportingContent = { Text(p.help, color = tokens.muted) },
                modifier = Modifier.clickable { onOpen(p) },
                colors = ListItemDefaults.colors(containerColor = tokens.surface),
            )
        }
    }
}

/** The agent's screens beside the five, opened over them. */
enum class AgentPage(val title: String, val help: String) {
    PROVIDERS("Providers", "Chat services a conversation can answer from"),
    SKILLS("Skills", "Instructions the agent follows when a job calls for them"),
    TASKS("Tasks", "Prompts that run on a schedule"),
    WORKSPACES("Workspaces", "Folders the agent works in"),
}
