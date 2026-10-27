package io.bigmoeonedge.app.ui

import android.annotation.SuppressLint
import android.graphics.BitmapFactory
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.compose.animation.animateContentSize
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import io.bigmoeonedge.app.agent.Agent
import io.bigmoeonedge.app.agent.AgentItem
import io.bigmoeonedge.app.agent.Block
import io.bigmoeonedge.app.agent.LocalFolders
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/** One item of a conversation, by its kind (docs/agent.md). */
@Composable
fun ItemView(item: AgentItem, isLast: Boolean, running: Boolean, onPreview: (String) -> Unit) {
    when (item.kind) {
        AgentItem.KIND_USER -> UserMessage(item, editable = !running)
        AgentItem.KIND_ASSISTANT -> AssistantMessage(item)
        AgentItem.KIND_TOOL -> if (item.awaitsApproval) ApprovalCard(item, onPreview) else ToolCard(item, onPreview)
        AgentItem.KIND_HELPER -> HelperRow(item)
        AgentItem.KIND_NOTICE -> NoticeRow(item)
        else -> {}
    }
}

/** The person's message, on the right: long-press to copy it, or to edit it and send again. */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun UserMessage(item: AgentItem, editable: Boolean) {
    val clipboard = LocalClipboardManager.current
    var menu by remember { mutableStateOf(false) }
    var editing by remember { mutableStateOf(false) }
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
        Box {
            Surface(
                color = tokens.accentTint, shape = Corner,
                modifier = Modifier.widthIn(max = 320.dp).clip(Corner).combinedClickable(onClick = {}, onLongClick = { menu = true }),
            ) {
                Text(item.text, style = MaterialTheme.typography.bodyLarge, modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp))
            }
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                DropdownMenuItem(text = { Text("Copy") }, onClick = { clipboard.setText(AnnotatedString(item.text)); menu = false })
                DropdownMenuItem(text = { Text("Edit and send again") }, enabled = editable, onClick = { editing = true; menu = false })
            }
        }
    }
    if (editing) {
        var text by remember(item.id) { mutableStateOf(item.text) }
        AlertDialog(
            onDismissRequest = { editing = false },
            title = { Text("Edit and send again") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Everything after this message is dropped, then the edited one is sent.", style = MaterialTheme.typography.bodySmall, color = tokens.muted)
                    OutlinedTextField(value = text, onValueChange = { text = it }, modifier = Modifier.fillMaxWidth(), minLines = 2, maxLines = 8)
                }
            },
            confirmButton = { TextButton(onClick = { Agent.editAndResend(item.id, text); editing = false }, enabled = text.isNotBlank()) { Text("Send") } },
            dismissButton = { TextButton(onClick = { editing = false }) { Text("Cancel") } },
        )
    }
}

/** The model's answer as Markdown, its thinking folded above it. */
@Composable
private fun AssistantMessage(item: AgentItem) {
    val clipboard = LocalClipboardManager.current
    Column(verticalArrangement = Arrangement.spacedBy(4.dp), modifier = Modifier.fillMaxWidth()) {
        if (item.reasoning.isNotEmpty()) ReasoningBlock(item.reasoning, openWhileStreaming = item.streaming && item.text.isEmpty())
        if (item.text.isNotEmpty()) SelectionContainer { MarkdownText(item.text) }
        if (!item.streaming && item.text.isNotEmpty()) {
            TextButton(
                onClick = { clipboard.setText(AnnotatedString(item.text)) },
                contentPadding = PaddingValues(horizontal = 8.dp, vertical = 0.dp),
            ) { Text("Copy", style = MaterialTheme.typography.labelMedium) }
        }
    }
}

/** The model's reasoning, dimmed and folded: open while it streams, closed once the answer starts. */
@Composable
private fun ReasoningBlock(reasoning: String, openWhileStreaming: Boolean) {
    var expanded by rememberSaveable { mutableStateOf<Boolean?>(null) }
    val open = expanded ?: openWhileStreaming
    Surface(color = tokens.surface2, shape = Corner, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp).animateContentSize()) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth().clickable { expanded = !open },
            ) {
                Icon(if (open) Icons.Filled.KeyboardArrowDown else Icons.Filled.KeyboardArrowRight, null, tint = tokens.muted, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(4.dp))
                Text("Thinking", style = MaterialTheme.typography.labelLarge, color = tokens.muted)
            }
            if (open) {
                SelectionContainer {
                    Text(reasoning, style = MaterialTheme.typography.bodySmall, color = tokens.muted, modifier = Modifier.padding(top = 4.dp))
                }
            }
        }
    }
}

/** A tool's state as an icon: a spinner while it runs, a check, a cross. */
@Composable
private fun StateIcon(state: String) {
    val m = Modifier.size(16.dp)
    when (state) {
        AgentItem.STATE_RUNNING -> CircularProgressIndicator(m, strokeWidth = 2.dp, color = tokens.accent)
        AgentItem.STATE_DONE -> Icon(Icons.Filled.Check, "done", tint = tokens.ok, modifier = m)
        AgentItem.STATE_DENIED -> Icon(Icons.Filled.Close, "denied", tint = tokens.warn, modifier = m)
        AgentItem.STATE_ERROR -> Icon(Icons.Filled.Close, "error", tint = tokens.err, modifier = m)
        AgentItem.STATE_WAITING -> Icon(Icons.Filled.Warning, "waiting", tint = tokens.warn, modifier = m)
        else -> Spacer(m)
    }
}

/** One tool call: its title in plain words and its state; open for what it read, wrote or answered. */
@Composable
fun ToolCard(item: AgentItem, onPreview: (String) -> Unit) {
    var open by rememberSaveable(item.id) { mutableStateOf(false) }
    val hasDetail = item.result.isNotEmpty() || item.diff.isNotEmpty() || item.args.isNotEmpty()
    Surface(color = tokens.surface, shape = Corner, border = androidx.compose.foundation.BorderStroke(1.dp, tokens.line), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.animateContentSize()) {
            Row(
                Modifier.fillMaxWidth().clickable(enabled = hasDetail) { open = !open }.padding(horizontal = 12.dp, vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                StateIcon(item.state)
                Text(
                    item.title.ifEmpty { item.tool }, style = MaterialTheme.typography.bodyMedium,
                    maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f),
                )
                when (item.state) {
                    AgentItem.STATE_DENIED -> Chip("denied", tokens.warn)
                    AgentItem.STATE_ERROR -> Chip("failed", tokens.err)
                }
                if (item.file.isNotEmpty()) TextButton(onClick = { onPreview(item.file) }, contentPadding = PaddingValues(horizontal = 8.dp)) { Text("Open") }
                if (hasDetail) Icon(if (open) Icons.Filled.KeyboardArrowDown else Icons.Filled.KeyboardArrowRight, null, tint = tokens.muted, modifier = Modifier.size(18.dp))
            }
            if (open) {
                HorizontalDivider(color = tokens.line)
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (item.diff.isNotEmpty()) DiffView(item.diff)
                    else if (item.args.isNotEmpty() && item.result.isEmpty()) CodeBox(prettyArgs(item.args))
                    if (item.result.isNotEmpty()) CodeBox(item.result)
                }
            }
        }
    }
}

/** Two or more tool calls in a row, under one line that counts them; open to see each. */
@Composable
fun RunGroup(run: Block.Run, onPreview: (String) -> Unit) {
    var open by rememberSaveable(run.items.first().id) { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth().animateContentSize(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(
            Modifier.fillMaxWidth().clip(Corner).clickable { open = !open }.padding(vertical = 6.dp, horizontal = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (run.running) CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp, color = tokens.accent)
            else Icon(if (open) Icons.Filled.KeyboardArrowDown else Icons.Filled.KeyboardArrowRight, null, tint = tokens.muted, modifier = Modifier.size(18.dp))
            Text(run.summary, style = MaterialTheme.typography.bodyMedium, color = tokens.muted, modifier = Modifier.weight(1f))
            if (run.failed > 0) Chip("${run.failed} failed", tokens.err)
        }
        if (open) run.items.forEach { ToolCard(it, onPreview) }
    }
}

/** A change the agent asked to make: nothing has run yet; it waits for Approve or Deny. */
@Composable
private fun ApprovalCard(item: AgentItem, onPreview: (String) -> Unit) {
    BmoeCard(colour = tokens.warnTint, padding = 12.dp) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Icon(Icons.Filled.Warning, null, tint = tokens.warn, modifier = Modifier.size(18.dp))
            Text("Approve this change?", style = MaterialTheme.typography.titleSmall, color = tokens.text)
        }
        Text(item.title.ifEmpty { item.tool }, style = MaterialTheme.typography.bodyMedium)
        when {
            item.diff.isNotEmpty() -> DiffView(item.diff, maxLines = 40)
            item.args.isNotEmpty() -> CodeBox(prettyArgs(item.args), maxLines = 20)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            if (item.file.isNotEmpty()) TextButton(onClick = { onPreview(item.file) }) { Text("Open file") }
            Spacer(Modifier.weight(1f))
            OutlinedButton(onClick = { Agent.answer(item.id, approve = false) }, shape = Corner) { Text("Deny") }
            Button(onClick = { Agent.answer(item.id, approve = true) }, shape = Corner) { Text("Approve") }
        }
    }
}

/** A helper the agent handed part of the job to: its task, its state and, once done, what it found. */
@Composable
private fun HelperRow(item: AgentItem) {
    Surface(color = tokens.surface2, shape = Corner, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.clickable(enabled = item.conversation.isNotEmpty()) { Agent.open(item.conversation) }.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                StateIcon(item.state)
                Text("Helper: " + item.title, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium, modifier = Modifier.weight(1f))
            }
            if (item.summary.isNotEmpty()) Text(item.summary, style = MaterialTheme.typography.bodySmall, color = tokens.muted, maxLines = 6, overflow = TextOverflow.Ellipsis)
        }
    }
}

/** Why something did not happen, in plain words, coloured by how much it matters. */
@Composable
private fun NoticeRow(item: AgentItem) {
    val (fg, bg) = when (item.level) {
        "error" -> tokens.err to tokens.errTint
        "warn" -> tokens.warn to tokens.warnTint
        else -> tokens.muted to tokens.surface2
    }
    Surface(color = bg, shape = Corner, modifier = Modifier.fillMaxWidth()) {
        Row(Modifier.padding(horizontal = 12.dp, vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.Top) {
            Icon(if (item.level == "info" || item.level.isEmpty()) Icons.Filled.Info else Icons.Filled.Warning, null, tint = fg, modifier = Modifier.size(16.dp).padding(top = 2.dp))
            Text(item.text, style = MaterialTheme.typography.bodySmall, color = tokens.text)
        }
    }
}

/** A unified diff: added lines on the ok tint, removed on the error tint, hunks muted. */
@Composable
fun DiffView(diff: String, maxLines: Int = 400) {
    val lines = diff.lines().let { if (it.size > maxLines) it.take(maxLines) + "… ${it.size - maxLines} more lines" else it }
    Surface(color = tokens.surface2, shape = Corner, modifier = Modifier.fillMaxWidth()) {
        // Every line as wide as the widest (and at least the card), so a line's tint is a band.
        BoxWithConstraints {
            Column(Modifier.horizontalScroll(rememberScrollState()).widthIn(min = maxWidth).width(IntrinsicSize.Max).padding(vertical = 6.dp)) {
            lines.forEach { l ->
                val (fg, bg) = when {
                    l.startsWith("+++") || l.startsWith("---") -> tokens.muted to null
                    l.startsWith("+") -> tokens.ok to tokens.okTint
                    l.startsWith("-") -> tokens.err to tokens.errTint
                    l.startsWith("@@") -> tokens.accentInk to null
                    else -> tokens.text to null
                }
                Text(
                    l.ifEmpty { " " }, style = CodeStyle.copy(fontSize = 12.sp), color = fg, softWrap = false,
                    modifier = (if (bg != null) Modifier.fillMaxWidth().background(bg) else Modifier.fillMaxWidth()).padding(horizontal = 10.dp),
                )
            }
            }
        }
    }
}

@Composable
private fun CodeBox(text: String, maxLines: Int = 200) {
    val shown = text.lines().let { if (it.size > maxLines) (it.take(maxLines) + "… ${it.size - maxLines} more lines").joinToString("\n") else text }
    Surface(color = tokens.surface2, shape = Corner, modifier = Modifier.fillMaxWidth()) {
        SelectionContainer {
            Text(shown, style = CodeStyle.copy(fontSize = 12.sp), softWrap = false, modifier = Modifier.horizontalScroll(rememberScrollState()).padding(10.dp))
        }
    }
}

private fun prettyArgs(args: String): String = runCatching { org.json.JSONObject(args).toString(2) }.getOrDefault(args)

/** What a file preview holds once read. */
private sealed class Preview {
    object Loading : Preview()
    data class Failed(val why: String) : Preview()
    data class Image(val bitmap: android.graphics.Bitmap) : Preview()
    data class Html(val html: String) : Preview()
    data class Markdown(val text: String) : Preview()
    data class Text(val text: String) : Preview()
    data class Binary(val type: String, val size: Int) : Preview()
}

private fun classify(path: String, bytes: ByteArray, contentType: String): Preview {
    val type = contentType.substringBefore(';').trim().lowercase()
    val ext = path.substringAfterLast('.', "").lowercase()
    return when {
        type.startsWith("image/") -> BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.let { Preview.Image(it) } ?: Preview.Binary(type, bytes.size)
        type == "text/html" || ext == "html" || ext == "htm" -> Preview.Html(String(bytes, Charsets.UTF_8))
        type == "text/markdown" || ext == "md" || ext == "markdown" -> Preview.Markdown(String(bytes, Charsets.UTF_8))
        type.startsWith("text/") || type.contains("json") || type.contains("xml") || type.contains("javascript") || looksLikeText(bytes) ->
            Preview.Text(String(bytes, Charsets.UTF_8))
        else -> Preview.Binary(type, bytes.size)
    }
}

/** No NUL byte in the first few kilobytes: a source file the server sent as octet-stream. */
private fun looksLikeText(b: ByteArray) = b.isNotEmpty() && b.take(4096).none { it == 0.toByte() }

/**
 * A file the agent produced, read through the server (`GET /api/conversations/<id>/file`): text and
 * code in the mono face, Markdown rendered, images drawn, HTML in a web view that is sandboxed from
 * the app (script runs inside the page only: no bridge to the app, no file or content access, no
 * navigation away). A file in one of the app's own folders can also be shared out.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FilePreviewSheet(path: String, workspacePath: String?, onDismiss: () -> Unit) {
    val ctx = LocalContext.current
    var preview by remember(path) { mutableStateOf<Preview>(Preview.Loading) }
    LaunchedEffect(path) {
        preview = withContext(Dispatchers.IO) {
            Agent.file(path).fold({ (bytes, type) -> classify(path, bytes, type) }, { Preview.Failed(it.message ?: "The file could not be read.") })
        }
    }
    val local = workspacePath?.let { File(it, path) }?.takeIf { it.isFile && LocalFolders.isLocal(ctx, it.path) }
    ModalBottomSheet(onDismissRequest = onDismiss, containerColor = tokens.surface, sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)) {
        Column(Modifier.fillMaxWidth().fillMaxHeight(0.9f).padding(horizontal = 16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(path, style = CodeStyle, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                if (local != null) IconButton(onClick = { ctx.startActivity(LocalFolders.shareIntent(ctx, local)) }) { Icon(Icons.Filled.Share, "Share") }
            }
            HorizontalDivider(color = tokens.line, modifier = Modifier.padding(vertical = 8.dp))
            when (val p = preview) {
                Preview.Loading -> LinearProgressIndicator(Modifier.fillMaxWidth())
                is Preview.Failed -> Text(p.why, color = tokens.err)
                is Preview.Image -> Image(p.bitmap.asImageBitmap(), contentDescription = path, modifier = Modifier.fillMaxWidth())
                is Preview.Html -> SandboxedHtml(p.html, Modifier.fillMaxSize())
                is Preview.Markdown -> Column(Modifier.verticalScroll(rememberScrollState())) { SelectionContainer { MarkdownText(p.text) } }
                is Preview.Text -> SelectionContainer {
                    Text(
                        p.text, style = CodeStyle, softWrap = false,
                        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()).horizontalScroll(rememberScrollState()),
                    )
                }
                is Preview.Binary -> Text("No preview for this kind of file (${p.type.ifEmpty { "unknown" }}, ${p.size} bytes).", color = tokens.muted)
            }
        }
    }
}

@SuppressLint("SetJavaScriptEnabled")
@Composable
private fun SandboxedHtml(html: String, modifier: Modifier) {
    AndroidView(
        modifier = modifier,
        factory = { c ->
            WebView(c).apply {
                // Script may run inside the page, which a page the agent made may need; it reaches
                // nothing of the app: no JavaScript interface is ever added, no file or content URI
                // is readable, and every navigation away from the page is refused.
                settings.javaScriptEnabled = true
                settings.allowFileAccess = false
                settings.allowContentAccess = false
                settings.domStorageEnabled = false
                settings.databaseEnabled = false
                settings.setGeolocationEnabled(false)
                settings.javaScriptCanOpenWindowsAutomatically = false
                settings.setSupportMultipleWindows(false)
                webViewClient = object : WebViewClient() {
                    override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest) = true
                }
                loadDataWithBaseURL(null, html, "text/html", "utf-8", null)
            }
        },
    )
}
