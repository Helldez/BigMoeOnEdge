package io.bigmoeonedge.app.engine

import org.json.JSONObject

/**
 * What `GET /api/conversations/<id>/system` answers (docs/server-api.md): the system prompt the server
 * sends before the conversation, exactly as sent, read-only (#625). [tokens] is what it takes of the
 * context, null with no model ready or on a provider; [external] when another agent program writes its own.
 */
data class SystemPrompt(val text: String, val tokens: Int?, val external: Boolean) {
    companion object {
        fun of(o: JSONObject) = SystemPrompt(
            text = o.optString("text"),
            tokens = if (o.has("tokens") && !o.isNull("tokens")) o.optInt("tokens") else null,
            external = o.optBoolean("external"),
        )
    }
}
