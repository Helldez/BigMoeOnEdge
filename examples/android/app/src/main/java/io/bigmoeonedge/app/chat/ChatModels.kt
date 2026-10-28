package io.bigmoeonedge.app.chat

import org.json.JSONArray
import org.json.JSONObject
import java.time.Instant
import java.time.LocalDateTime
import java.time.OffsetDateTime
import java.time.ZoneId

/**
 * A conversation's shapes as the server sends them (docs/agent.md). The app starts its server with
 * `--chat-only`, so a conversation here is plain chat: the person's messages, the model's answers
 * and the server's notices. Decoded tolerantly: a field the server leaves out reads as its empty
 * value, so a server a little older or newer than the app still draws a screen instead of failing
 * a parse, and a kind this app does not draw is skipped.
 */

/** One entry of a conversation, upserted by [id]. */
data class ChatItem(
    val id: String,
    val kind: String,
    val text: String = "",
    val reasoning: String = "",
    val streaming: Boolean = false,
    /** A notice's weight: info, warn or error. */
    val level: String = "",
) {
    companion object {
        const val KIND_USER = "user"
        const val KIND_ASSISTANT = "assistant"
        const val KIND_NOTICE = "notice"

        fun of(o: JSONObject): ChatItem = ChatItem(
            id = o.optString("id"),
            kind = o.optString("kind"),
            text = o.str("text"),
            reasoning = o.str("reasoning"),
            streaming = o.optBoolean("streaming", false),
            level = o.str("level"),
        )

        fun list(a: JSONArray?): List<ChatItem> =
            a?.let { arr -> (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.let(::of) } }.orEmpty()
    }
}

/** A row of the conversation list. [state]: idle, running or error. */
data class ConversationSummary(
    val id: String,
    val title: String,
    val state: String = STATE_IDLE,
    /** Milliseconds since the epoch; 0 when the server sent none. */
    val updated: Long = 0,
) {
    val busy get() = state == STATE_RUNNING

    companion object {
        const val STATE_IDLE = "idle"
        const val STATE_RUNNING = "running"
        const val STATE_ERROR = "error"

        fun of(o: JSONObject): ConversationSummary = ConversationSummary(
            id = o.optString("id"),
            title = o.str("title"),
            state = o.str("state").ifEmpty { STATE_IDLE },
            updated = timeMs(o.opt("updated")),
        )

        fun list(a: JSONArray?): List<ConversationSummary> =
            a?.let { arr -> (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.let(::of) } }.orEmpty()
    }
}

/** A conversation with its items: `GET /api/conversations/<id>`. */
data class ConversationFull(val summary: ConversationSummary, val items: List<ChatItem>) {
    companion object {
        fun of(o: JSONObject) = ConversationFull(ConversationSummary.of(o), ChatItem.list(o.optJSONArray("items")))
    }
}

/** A string field, with JSON null and absence both read as "". */
internal fun JSONObject.str(key: String): String = if (isNull(key)) "" else optString(key, "")

/**
 * A time the server sent, as milliseconds since the epoch: epoch seconds, epoch milliseconds or an
 * ISO 8601 string are all taken, since the contract names the fields and not their encoding.
 */
fun timeMs(v: Any?): Long = when (v) {
    null, JSONObject.NULL -> 0L
    is Number -> v.toLong().let { if (it in 1..99_999_999_999L) it * 1000 else it }
    is String -> v.trim().let { s ->
        s.toLongOrNull()?.let { timeMs(it) }
            ?: runCatching { Instant.parse(s).toEpochMilli() }.getOrNull()
            ?: runCatching { OffsetDateTime.parse(s).toInstant().toEpochMilli() }.getOrNull()
            ?: runCatching { LocalDateTime.parse(s).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli() }.getOrNull()
            ?: 0L
    }
    else -> 0L
}
