package io.bigmoeonedge.app.ui

import androidx.compose.animation.animateContentSize
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.unit.dp
import io.bigmoeonedge.app.chat.Chat
import io.bigmoeonedge.app.chat.ChatItem

/**
 * One item of a conversation, by its kind (docs/agent.md). [answerAgain]: this is the last answer,
 * which offers Answer again; [running]: an answer is being written, so nothing is resent meanwhile.
 */
@Composable
fun ItemView(item: ChatItem, answerAgain: Boolean, running: Boolean) {
    when (item.kind) {
        ChatItem.KIND_USER -> UserMessage(item, editable = !running)
        ChatItem.KIND_ASSISTANT -> AssistantMessage(item, answerAgain = answerAgain, enabled = !running)
        ChatItem.KIND_NOTICE -> NoticeRow(item)
        else -> {}
    }
}

/** The person's message, on the right: long-press to copy it, or to edit it and send again. */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun UserMessage(item: ChatItem, editable: Boolean) {
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
            confirmButton = { TextButton(onClick = { Chat.editAndResend(item.id, text); editing = false }, enabled = text.isNotBlank()) { Text("Send") } },
            dismissButton = { TextButton(onClick = { editing = false }) { Text("Cancel") } },
        )
    }
}

/** The model's answer as Markdown, its thinking folded above it; Copy, and Answer again on the last one. */
@Composable
private fun AssistantMessage(item: ChatItem, answerAgain: Boolean, enabled: Boolean) {
    val clipboard = LocalClipboardManager.current
    Column(verticalArrangement = Arrangement.spacedBy(4.dp), modifier = Modifier.fillMaxWidth()) {
        if (item.reasoning.isNotEmpty()) ReasoningBlock(item.reasoning, openWhileStreaming = item.streaming && item.text.isEmpty())
        if (item.text.isNotEmpty()) SelectionContainer { ChatMarkdown(item.text) }
        if (!item.streaming) {
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                if (item.text.isNotEmpty()) {
                    TextButton(
                        onClick = { clipboard.setText(AnnotatedString(item.text)) },
                        contentPadding = PaddingValues(horizontal = 8.dp, vertical = 0.dp),
                    ) { Text("Copy", style = MaterialTheme.typography.labelMedium) }
                }
                if (answerAgain) {
                    TextButton(
                        onClick = { Chat.answerAgain() }, enabled = enabled,
                        contentPadding = PaddingValues(horizontal = 8.dp, vertical = 0.dp),
                    ) { Text("Answer again", style = MaterialTheme.typography.labelMedium) }
                }
            }
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

/** Why something did not happen, in plain words, coloured by how much it matters. */
@Composable
private fun NoticeRow(item: ChatItem) {
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
