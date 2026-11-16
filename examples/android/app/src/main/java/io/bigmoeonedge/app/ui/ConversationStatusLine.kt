package io.bigmoeonedge.app.ui

import android.provider.Settings
import androidx.compose.animation.core.EaseInOut
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.bigmoeonedge.app.R
import io.bigmoeonedge.app.chat.Chat
import io.bigmoeonedge.app.chat.ChatItem
import io.bigmoeonedge.app.chat.ConversationStatus
import io.bigmoeonedge.app.chat.ConversationStatus.Mood
import io.bigmoeonedge.app.chat.ConversationSummary
import io.bigmoeonedge.app.engine.Engine

/**
 * What the engine and the open conversation are doing, in words ([ConversationStatus]); [greet] for
 * the welcome of an empty conversation.
 */
@Composable
fun rememberConversationStatus(greet: Boolean = false): ConversationStatus {
    val engine by Engine.state.collectAsStateWithLifecycle()
    val chat by Chat.state.collectAsStateWithLifecycle()
    val last = chat.items.lastOrNull()
    val running = chat.running || chat.sending
    // A live token counts only while this turn runs and before its summary (avg < 0 until done).
    val live = engine.live.takeIf { running && it.avgTokensPerSecond < 0 }
    return ConversationStatus.of(
        ConversationStatus.Facts(
            serverUp = engine.serverUp,
            infoKnown = engine.info != null,
            engineState = engine.info?.state.orEmpty(),
            running = running,
            writing = last?.kind == ChatItem.KIND_ASSISTANT && last.text.isNotEmpty(),
            lastIsAnswer = last?.kind == ChatItem.KIND_ASSISTANT,
            failed = chat.open?.state == ConversationSummary.STATE_ERROR,
            cancelled = engine.lastDone?.optBoolean("cancelled", false) == true,
            flashWaitMs = live?.flashWaitMs ?: 0.0,
            wallMs = live?.wallMs ?: 0.0,
        ),
        greet = greet,
    )
}

/** The logo as the status's face: breathing while it works, dimmed asleep, still otherwise. */
@Composable
fun StatusLogo(mood: Mood, size: Dp, modifier: Modifier = Modifier) {
    Icon(
        painterResource(R.drawable.ic_logo), contentDescription = null, tint = Color.Unspecified,
        modifier = modifier.size(size).alpha(logoAlpha(mood)),
    )
}

/** The status's words: muted, or warn-coloured when something needs the person. */
@Composable
fun statusColour(mood: Mood): Color = if (mood == Mood.ALERT) tokens.warn else tokens.muted

/**
 * The top of a conversation that has begun: the logo beside one line of plain words. An empty
 * conversation carries the same words under the welcome's logo instead (as the web UI does).
 */
@Composable
fun ConversationStatusLine() {
    val status = rememberConversationStatus()
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.padding(horizontal = 16.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            StatusLogo(status.mood, 32.dp)
            Spacer(Modifier.width(10.dp))
            Text(
                status.text, fontSize = 14.sp, fontWeight = LabelWeight, maxLines = 1, overflow = TextOverflow.Ellipsis,
                color = statusColour(status.mood),
                modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
            )
        }
        HorizontalDivider(color = tokens.line)
    }
}

/** Busy, the logo breathes (1 to 0.45 over 1.4 s, there and back); asleep, it is dimmed; otherwise still. */
@Composable
private fun logoAlpha(mood: Mood): Float {
    val context = LocalContext.current
    val reduced = remember(context) {
        runCatching { Settings.Global.getFloat(context.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f }.getOrDefault(false)
    }
    if (mood == Mood.SLEEP) return 0.7f
    if (mood != Mood.BUSY || reduced) return 1f
    val breath by rememberInfiniteTransition(label = "breath").animateFloat(
        initialValue = 1f, targetValue = 0.45f,
        animationSpec = infiniteRepeatable(tween(1400, easing = EaseInOut), RepeatMode.Reverse),
        label = "breath",
    )
    return breath
}
