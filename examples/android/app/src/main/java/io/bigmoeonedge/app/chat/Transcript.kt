package io.bigmoeonedge.app.chat

import org.json.JSONObject

/**
 * The open conversation as the events build it: pure, so the rules the transcript depends on
 * (where an item lands, what a delta adds, what Answer again resends) are tested without a server.
 */
object ItemReducer {
    /** An `item` event: the item whole. It replaces the one with its id in place, or is appended. */
    fun upsert(items: List<ChatItem>, item: ChatItem): List<ChatItem> {
        val i = items.indexOfFirst { it.id == item.id }
        if (i < 0) return items + item
        return items.toMutableList().also { it[i] = item }
    }

    /**
     * A `delta` event: what a streaming assistant item adds. A delta for an item not seen yet (the
     * stream was joined mid-answer, or its `item` event is still on the way) starts it.
     */
    fun delta(items: List<ChatItem>, id: String, text: String?, reasoning: String?): List<ChatItem> {
        val i = items.indexOfFirst { it.id == id }
        if (i < 0) {
            return items + ChatItem(id, ChatItem.KIND_ASSISTANT, text = text.orEmpty(), reasoning = reasoning.orEmpty(), streaming = true)
        }
        val cur = items[i]
        val next = cur.copy(text = cur.text + text.orEmpty(), reasoning = cur.reasoning + reasoning.orEmpty())
        return items.toMutableList().also { it[i] = next }
    }

    /** An event's effect on the open conversation [openId]: another conversation's events change nothing here. */
    fun apply(openId: String?, items: List<ChatItem>, event: String, data: JSONObject): List<ChatItem> {
        if (openId == null || data.optString("conversation") != openId) return items
        return when (event) {
            "item" -> data.optJSONObject("item")?.let { upsert(items, ChatItem.of(it)) } ?: items
            "delta" -> delta(
                items, data.optString("item"),
                data.takeIf { it.has("text") && !it.isNull("text") }?.optString("text"),
                data.takeIf { it.has("reasoning") && !it.isNull("reasoning") }?.optString("reasoning"),
            )
            else -> items
        }
    }

    /**
     * A `conversation` event on the list: the row's title, state and time change; an id not listed
     * yet (a conversation another client opened) goes to the top. Newest first.
     */
    fun conversation(list: List<ConversationSummary>, data: JSONObject): List<ConversationSummary> {
        val id = data.optString("id").ifEmpty { return list }
        val i = list.indexOfFirst { it.id == id }
        val base = if (i >= 0) list[i] else ConversationSummary(id, "")
        val next = base.copy(
            title = if (data.has("title")) data.str("title") else base.title,
            state = if (data.has("state")) data.str("state").ifEmpty { base.state } else base.state,
            updated = if (data.has("updated")) timeMs(data.opt("updated")).takeIf { it > 0 } ?: base.updated else base.updated,
        )
        val rest = list.filterIndexed { j, _ -> j != i }
        // Without a time the row the event names is the latest one: it goes first as it is.
        return if (next.updated == 0L) listOf(next) + rest else (listOf(next) + rest).sortedByDescending { it.updated }
    }

    /** The person's last message: what Answer again resends, through the edit route, as it was. */
    fun lastUser(items: List<ChatItem>): ChatItem? = items.lastOrNull { it.kind == ChatItem.KIND_USER }

    /**
     * The item that carries Answer again: the last assistant answer, once it is whole, when nothing
     * but notices follows it (a newer question has its own answer coming).
     */
    fun answerAgainOn(items: List<ChatItem>): String? {
        val last = items.lastOrNull { it.kind != ChatItem.KIND_NOTICE } ?: return null
        if (last.kind != ChatItem.KIND_ASSISTANT || last.streaming || lastUser(items) == null) return null
        return last.id
    }

    /** The items an edit of [itemId] leaves: everything before it (the server drops it and what follows). */
    fun before(items: List<ChatItem>, itemId: String): List<ChatItem> {
        val i = items.indexOfFirst { it.id == itemId }
        return if (i < 0) items else items.take(i)
    }

    /** The body of `POST /api/conversations/<id>/edit`: the message [itemId] replaced by [text]. */
    fun editBody(itemId: String, text: String): JSONObject = JSONObject().put("item", itemId).put("text", text)
}
