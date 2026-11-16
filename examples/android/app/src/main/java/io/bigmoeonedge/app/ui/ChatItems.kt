package io.bigmoeonedge.app.ui

import androidx.compose.animation.animateContentSize
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.compose.ui.unit.dp
import io.bigmoeonedge.app.chat.Chat
import io.bigmoeonedge.app.chat.ChatItem
import io.bigmoeonedge.app.chat.TurnFigures
import io.bigmoeonedge.app.chat.TurnSummary
import io.bigmoeonedge.app.chat.Words
import io.bigmoeonedge.app.engine.Engine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.runningFold

/**
 * One item of a conversation, by its kind (docs/agent.md). [answerAgain]: this is the last answer,
 * which offers Answer again; [running]: an answer is being written, so nothing is resent meanwhile;
 * [figures]: how this answer ran, when the engine said so; [expectedTokS]: the rate the plan predicts.
 */
@Composable
fun ItemView(
    item: ChatItem,
    answerAgain: Boolean,
    running: Boolean,
    figures: TurnSummary? = null,
    expectedTokS: Double? = null,
    nav: ChatNav? = null,
) {
    when (item.kind) {
        ChatItem.KIND_USER -> UserMessage(item, editable = !running)
        ChatItem.KIND_ASSISTANT -> AssistantMessage(item, answerAgain = answerAgain, enabled = !running, figures, expectedTokS, nav)
        ChatItem.KIND_NOTICE -> NoticeRow(item)
        else -> {}
    }
}

/**
 * The person's message, on the right, its images above its words: long-press to copy it, or to
 * edit it and send again. An edit changes the words; the images go again as they were.
 */
@OptIn(ExperimentalFoundationApi::class, ExperimentalLayoutApi::class)
@Composable
private fun UserMessage(item: ChatItem, editable: Boolean) {
    val clipboard = LocalClipboardManager.current
    var menu by remember { mutableStateOf(false) }
    var editing by remember { mutableStateOf(false) }
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
        Box {
            Surface(
                color = tokens.shellHi, contentColor = tokens.onShell, shape = BubbleCorner,
                modifier = Modifier.widthIn(max = 320.dp).clip(BubbleCorner).combinedClickable(onClick = {}, onLongClick = { menu = true }),
            ) {
                Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (item.images.isNotEmpty()) {
                        FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            item.images.forEach { src -> ImageThumb(src, MessageThumb) }
                        }
                    }
                    if (item.text.isNotEmpty()) Text(item.text, style = MaterialTheme.typography.bodyLarge, color = tokens.onShell)
                }
            }
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                // Copy copies the words: an item of images alone has none to copy.
                if (item.text.isNotEmpty()) {
                    DropdownMenuItem(text = { Text("Copy") }, onClick = { clipboard.setText(AnnotatedString(item.text)); menu = false })
                }
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
                    Text(
                        if (item.images.isEmpty()) "Everything after this message is dropped, then the edited one is sent."
                        else "Everything after this message is dropped, then the edited one is sent, with the same images.",
                        style = MaterialTheme.typography.bodySmall, color = tokens.muted,
                    )
                    OutlinedTextField(value = text, onValueChange = { text = it }, modifier = Modifier.fillMaxWidth(), minLines = 2, maxLines = 8)
                }
            },
            confirmButton = {
                TextButton(
                    onClick = { Chat.editAndResend(item.id, text, item.images); editing = false },
                    enabled = text.isNotBlank() || item.images.isNotEmpty(),
                ) { Text("Send") }
            },
            dismissButton = { TextButton(onClick = { editing = false }) { Text("Cancel") } },
        )
    }
}

/**
 * The model's answer on a white sheet, full width, its thinking folded above it; Copy, and Answer
 * again on the last one; then one quiet line of how it ran (`tg … · pp …`), live while it streams,
 * which opens to every figure in plain words.
 */
@Composable
private fun AssistantMessage(item: ChatItem, answerAgain: Boolean, enabled: Boolean, figures: TurnSummary?, expectedTokS: Double?, nav: ChatNav?) {
    val clipboard = LocalClipboardManager.current
    Surface(color = tokens.surface, shape = SheetCorner, shadowElevation = SheetElevation, modifier = Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(4.dp), modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp)) {
            if (item.reasoning.isNotEmpty()) ReasoningBlock(item.reasoning, openWhileStreaming = item.streaming && item.text.isEmpty())
            if (item.text.isNotEmpty()) SelectionContainer { ChatMarkdown(item.text) }
            if (item.streaming) LiveRate()
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
                if (figures != null) TurnFiguresView(figures, expectedTokS, nav)
            }
        }
    }
}

/** While an answer is written: its writing speed, live, where the finished answer's line will land. */
@Composable
private fun LiveRate() {
    // Smoothed over the tokens so far, as the web UI's: one token's rate alone jumps about.
    val live by remember {
        Engine.state.map { it.live }.filter { it.avgTokensPerSecond < 0 && it.wallMs > 0 }
            .map { it.step to it.tokensPerSecond }.distinctUntilChanged()
            .runningFold(0.0) { prev, (_, tps) -> TurnFigures.ema(prev.takeIf { it > 0 }, tps) }
    }.collectAsStateWithLifecycle(0.0)
    if (live <= 0) return
    Text(
        TurnFigures.live(live), style = FigureStyle, fontWeight = LabelWeight, color = tokens.muted,
        modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
    )
}

/**
 * How a finished answer ran: one quiet line, tg (the writing speed) and pp (the prompt-reading speed),
 * which opens to every figure with what it measures and the way to the plan and to Metrics.
 */
@Composable
private fun TurnFiguresView(figures: TurnSummary, expectedTokS: Double?, nav: ChatNav?) {
    var open by rememberSaveable { mutableStateOf(false) }
    Column(Modifier.animateContentSize()) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.clip(Corner).clickable(onClickLabel = Words.FIGURES_OPEN) { open = !open }
                .heightIn(min = 32.dp).padding(horizontal = 8.dp),
        ) {
            Text(TurnFigures.line(figures.summary), style = FigureStyle, fontWeight = LabelWeight, color = tokens.muted)
            Icon(
                if (open) Icons.Filled.KeyboardArrowUp else Icons.Filled.KeyboardArrowDown, contentDescription = Words.FIGURES_OPEN,
                tint = tokens.muted, modifier = Modifier.padding(start = 4.dp).size(18.dp),
            )
        }
        if (open) {
            Surface(
                color = tokens.surface, shape = SheetCorner, border = BorderStroke(1.dp, tokens.line), shadowElevation = 1.dp,
                modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
            ) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    TurnFigures.of(figures.summary, figures.nCtx, expectedTokS).forEach { f ->
                        // A long value (the time split) goes on its own line under the label, left-aligned,
                        // instead of wrapping as a ragged column beside it.
                        val own = f.value.length > TurnFigures.INLINE_VALUE_MAX
                        Column {
                            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                Text(f.label, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
                                if (f.value.isNotEmpty() && !own) Text(f.value, style = FigureStyle, fontWeight = FontWeight.Bold, color = tokens.text)
                            }
                            if (own) Text(f.value, style = FigureStyle, fontWeight = FontWeight.Bold, color = tokens.text, modifier = Modifier.padding(top = 2.dp))
                            Text(f.hint, style = MaterialTheme.typography.bodySmall, color = tokens.muted, modifier = Modifier.padding(top = 2.dp))
                        }
                    }
                }
            }
            if (nav != null) {
                Row {
                    TextButton(onClick = nav.openPlan, contentPadding = PaddingValues(horizontal = 8.dp)) { Text(Words.TO_PLAN) }
                    TextButton(onClick = nav.openMetrics, contentPadding = PaddingValues(horizontal = 8.dp)) { Text(Words.TO_METRICS) }
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
