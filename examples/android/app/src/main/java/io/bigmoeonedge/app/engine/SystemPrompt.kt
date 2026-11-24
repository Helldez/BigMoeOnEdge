package io.bigmoeonedge.app.engine

import org.json.JSONObject

/**
 * What `GET /api/conversations/<id>/system` answers (docs/server-api.md): the system prompt the model
 * reads before the conversation, exactly as sent (#625): the agent's lines where it has a folder,
 * then [own], the person's. [tokens] is what it takes of the context, null with no model ready or on
 * a provider; [external] when another agent program writes its own.
 */
data class SystemPrompt(val text: String, val own: String, val tokens: Int?, val external: Boolean) {
    /** What the server puts before the person's own text; "" where it adds nothing. */
    val added get() = if (text == own) "" else text

    companion object {
        fun of(o: JSONObject) = SystemPrompt(
            text = o.optString("text"),
            own = o.optString("own"),
            tokens = if (o.has("tokens") && !o.isNull("tokens")) o.optInt("tokens") else null,
            external = o.optBoolean("external"),
        )
    }
}
