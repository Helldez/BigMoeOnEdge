package io.bigmoeonedge.app.data

import io.bigmoeonedge.app.engine.ChatMessage
import io.bigmoeonedge.app.engine.ChoiceScore
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.UUID

/** A conversation as the app keeps it. The server holds none: every request carries it whole. */
data class Conversation(
    val id: String,
    val title: String,
    val createdAt: Long,
    val updatedAt: Long,
    val model: String, // the model it was last answered by, for the list
    val messages: List<ChatMessage>,
) {
    fun toJson(): JSONObject {
        val arr = JSONArray()
        for (m in messages) {
            val o = JSONObject().put("role", m.role).put("content", m.content)
                .put("reasoning", m.reasoning).put("metrics", m.metrics)
            m.choices?.let { cs ->
                val arr2 = JSONArray()
                for (c in cs) arr2.put(JSONObject().put("label", c.label).put("text", c.text).put("prob", c.prob))
                o.put("choices", arr2).put("best", m.best)
            }
            arr.put(o)
        }
        return JSONObject().put("id", id).put("title", title).put("created_at", createdAt)
            .put("updated_at", updatedAt).put("model", model).put("messages", arr)
    }

    companion object {
        fun fromJson(o: JSONObject): Conversation {
            val arr = o.optJSONArray("messages") ?: JSONArray()
            val messages = (0 until arr.length()).map { i ->
                val m = arr.getJSONObject(i)
                val choices = m.optJSONArray("choices")?.let { cs ->
                    (0 until cs.length()).map { j ->
                        val c = cs.getJSONObject(j)
                        ChoiceScore(c.optString("label"), c.optString("text"), c.optDouble("prob", 0.0))
                    }
                }
                ChatMessage(
                    m.optString("role", "user"), m.optString("content"), m.optString("reasoning"), m.optString("metrics"),
                    choices, m.optInt("best", -1),
                )
            }
            return Conversation(
                id = o.getString("id"),
                title = o.optString("title", ""),
                createdAt = o.optLong("created_at", 0),
                updatedAt = o.optLong("updated_at", 0),
                model = o.optString("model", ""),
                messages = messages,
            )
        }

        fun fresh(): Conversation {
            val now = System.currentTimeMillis()
            return Conversation(UUID.randomUUID().toString(), "", now, now, "", emptyList())
        }

        /** A title from the first thing the user said: the first line, cut to fit a list row. */
        fun titleFor(firstUserText: String): String {
            val line = firstUserText.lineSequence().map { it.trim() }.firstOrNull { it.isNotEmpty() } ?: "New chat"
            return if (line.length <= 48) line else line.take(47).trimEnd() + "…"
        }
    }
}

/** What the conversation list shows of each: no messages, so a long history lists quickly. */
data class ConversationMeta(val id: String, val title: String, val updatedAt: Long, val messageCount: Int, val model: String)

/**
 * One JSON file per conversation under the app's own files directory. A conversation is small
 * (text), written whole on every change, and read whole when opened; the list reads only what
 * the rows need. No database: there is nothing to query, and a file is what survives the app
 * being killed and reinstalled over.
 */
class ConversationStore(private val dir: File) {
    init {
        dir.mkdirs()
    }

    fun list(): List<ConversationMeta> =
        dir.listFiles { f -> f.isFile && f.name.endsWith(".json") }.orEmpty()
            .mapNotNull { f -> runCatching { load(f.name.removeSuffix(".json")) }.getOrNull() }
            .map { c -> ConversationMeta(c.id, c.title.ifEmpty { "New chat" }, c.updatedAt, c.messages.size, c.model) }
            .sortedByDescending { it.updatedAt }

    fun load(id: String): Conversation? {
        val f = fileFor(id)
        if (!f.isFile) return null
        return runCatching { Conversation.fromJson(JSONObject(f.readText())) }.getOrNull()
    }

    /** Written to a sibling and renamed, so a kill mid-write never leaves a half conversation. */
    fun save(c: Conversation) {
        val f = fileFor(c.id)
        val tmp = File(dir, c.id + ".json.tmp")
        tmp.writeText(c.toJson().toString())
        if (!tmp.renameTo(f)) {
            f.delete()
            tmp.renameTo(f)
        }
    }

    fun delete(id: String) {
        fileFor(id).delete()
    }

    private fun fileFor(id: String): File {
        require(id.all { it.isLetterOrDigit() || it == '-' }) { "conversation id" }
        return File(dir, "$id.json")
    }
}
