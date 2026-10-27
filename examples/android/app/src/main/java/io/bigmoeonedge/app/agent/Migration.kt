package io.bigmoeonedge.app.agent

import io.bigmoeonedge.app.data.Conversation
import io.bigmoeonedge.app.data.ConversationStore
import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale

/** The two calls the migration makes, so a test hands it a fake server. */
interface ConversationCreator {
    /** `POST /api/conversations`: answers the conversation the server made. */
    fun createConversation(body: JSONObject): JSONObject

    fun deleteConversation(id: String)
}

/** What the app kept before the server held conversations, as the migration reads it. */
interface LocalConversations {
    fun ids(): List<String>
    fun load(id: String): Conversation?
    fun delete(id: String)
}

/** The app's conversation folder, read through the store that wrote it. */
class StoreConversations(private val store: ConversationStore) : LocalConversations {
    override fun ids() = store.list().map { it.id }
    override fun load(id: String) = store.load(id)
    override fun delete(id: String) = store.delete(id)
}

data class MigrationResult(val moved: Int, val kept: Int, val error: String? = null)

/**
 * The conversations this app used to keep on the phone, handed to the server once: each becomes a
 * server conversation with its messages as items, and the phone's copy is deleted only once the
 * server's answer shows every item arrived. A server that ignored the items (an older one) gets its
 * empty conversation deleted again and the phone keeps its copy, to try again on a later launch:
 * nothing is lost for a server that cannot take it.
 */
object Migration {
    fun run(local: LocalConversations, server: ConversationCreator): MigrationResult {
        var moved = 0
        var kept = 0
        var error: String? = null
        for (id in local.ids()) {
            val c = local.load(id) ?: continue
            val items = itemsOf(c)
            if (items.length() == 0) {
                // An empty conversation (a new chat that was never sent) has nothing to carry over.
                local.delete(id)
                continue
            }
            try {
                val made = server.createConversation(body(c, items))
                val got = made.optJSONArray("items")?.length() ?: -1
                if (got == items.length()) {
                    local.delete(id)
                    moved++
                } else {
                    made.optString("id").takeIf { it.isNotEmpty() }?.let { runCatching { server.deleteConversation(it) } }
                    kept++
                }
            } catch (t: Throwable) {
                // The server is down or refused: try the rest next launch, all of it kept.
                error = t.message ?: t.javaClass.simpleName
                kept++
                break
            }
        }
        return MigrationResult(moved, kept, error)
    }

    fun body(c: Conversation, items: JSONArray = itemsOf(c)): JSONObject =
        JSONObject().put("title", c.title.ifEmpty { Conversation.titleFor(c.messages.firstOrNull()?.content.orEmpty()) }).put("items", items)

    /** The messages as `{kind, text}` items; a decision's answer becomes its options in words. */
    fun itemsOf(c: Conversation): JSONArray {
        val a = JSONArray()
        for (m in c.messages) {
            val kind = if (m.role == "user") AgentItem.KIND_USER else AgentItem.KIND_ASSISTANT
            val text = m.choices?.let { cs ->
                cs.mapIndexed { i, s ->
                    val pick = if (i == m.best) " (picked)" else ""
                    String.format(Locale.US, "%s) %s: %.0f%%%s", s.label, s.text, 100.0 * s.prob, pick)
                }.joinToString("\n")
            } ?: m.content
            if (text.isBlank()) continue
            a.put(JSONObject().put("kind", kind).put("text", text))
        }
        return a
    }
}
