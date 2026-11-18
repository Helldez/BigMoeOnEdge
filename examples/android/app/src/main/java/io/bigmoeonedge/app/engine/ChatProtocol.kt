package io.bigmoeonedge.app.engine

import org.json.JSONArray
import org.json.JSONObject

/**
 * One message of a conversation as this app keeps it. [reasoning] is the model's thinking span
 * on an assistant turn (shown folded above the answer, and sent back to the model as
 * `reasoning_content` so a reasoning model sees its own trace); [metrics] is the one-line figure
 * shown under the answer. What the app kept on the phone before the server held conversations;
 * the migration reads it, and a decision still uses [ChoiceScore].
 */
data class ChatMessage(
    val role: String,
    val content: String,
    val reasoning: String = "",
    val metrics: String = "",
    /** A decision's answer: every option with its probability, [best] the one the model picked. */
    val choices: List<ChoiceScore>? = null,
    val best: Int = -1,
)

/** One option of a decision and the probability the model put on it. */
data class ChoiceScore(val label: String, val text: String, val prob: Double)

/**
 * One streamed chunk of `POST /v1/chat/completions`, reduced to what the UI applies. [content] and
 * [reasoning] are deltas to append, unless [reset] says they are the whole text so far and must
 * replace what was shown (the server's chat parser reclassified text it had already sent, a closing
 * reasoning tag). [finish] is set on the last choice chunk, with the run [summary] beside it.
 */
data class ChatChunk(
    val content: String? = null,
    val reasoning: String? = null,
    val reset: Boolean = false,
    val finish: String? = null,
    val summary: JSONObject? = null,
    val error: String? = null,
    val errorCode: String = "",
)

/**
 * The OpenAI chat-completions protocol as this client speaks it: the request body for a
 * conversation, and each streamed chunk read back. JSON in, JSON out, no network, so it is tested
 * without a server. What the protocol means for a model is the server's business.
 */
object ChatProtocol {
    const val DONE = "[DONE]"

    /**
     * The whole conversation, every time: the server reuses what its KV cache already holds. A
     * decision is not a conversation turn, so a question with a chosen answer stays out.
     */
    fun requestBody(messages: List<ChatMessage>, maxTokens: Int? = null, think: Boolean? = null): JSONObject {
        val arr = JSONArray()
        val sent = mutableListOf<ChatMessage>()
        for (m in messages) {
            if (m.choices != null) {
                if (sent.lastOrNull()?.role == "user") sent.removeAt(sent.lastIndex)
                continue
            }
            sent += m
        }
        for (m in sent) {
            val o = JSONObject().put("role", m.role).put("content", m.content)
            if (m.role == "assistant" && m.reasoning.isNotEmpty()) o.put("reasoning_content", m.reasoning)
            arr.put(o)
        }
        val body = JSONObject().put("messages", arr).put("stream", true)
        if (maxTokens != null) body.put("max_tokens", maxTokens)
        if (think != null) body.put("think", think)
        return body
    }

    /** One `data:` payload; null for the `[DONE]` sentinel or anything that is not a chunk. */
    fun parseChunk(data: String): ChatChunk? {
        val t = data.trim()
        if (t.isEmpty() || t == DONE) return null
        val o = runCatching { JSONObject(t) }.getOrNull() ?: return null
        o.optJSONObject("error")?.let { e ->
            return ChatChunk(
                error = e.optString("message").ifEmpty { "the server reported an error" },
                errorCode = e.optString("code"),
            )
        }
        val choice = o.optJSONArray("choices")?.optJSONObject(0)
        val bmoe = o.optJSONObject("bmoe")
        if (choice == null) return ChatChunk(summary = bmoe?.optJSONObject("summary"))
        val delta = choice.optJSONObject("delta")
        val finish = choice.optString("finish_reason").takeIf { it.isNotEmpty() && !choice.isNull("finish_reason") }
        return ChatChunk(
            content = delta?.takeIf { it.has("content") && !it.isNull("content") }?.getString("content"),
            reasoning = delta?.takeIf { it.has("reasoning_content") && !it.isNull("reasoning_content") }
                ?.getString("reasoning_content"),
            reset = bmoe?.optBoolean("reset", false) == true,
            finish = finish,
            summary = bmoe?.optJSONObject("summary"),
        )
    }
}
