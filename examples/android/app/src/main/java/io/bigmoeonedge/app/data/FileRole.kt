package io.bigmoeonedge.app.data

/**
 * What a gguf file is for, as the server reports it (`role`, docs/server-api.md, "Models"): a model
 * to chat with, or a companion file that only goes with one. The server decides, from the header on
 * this phone and on inspect, from the name in a repository listing; this only names the answer and
 * keeps the companions apart so they do not read as models. The web UI does the same (ui/src/lib/roles.ts).
 */
object FileRole {
    /** True for a file the server says is not a model to load. "" (an unreadable header) is not known to be one. */
    fun isCompanion(role: String): Boolean = role.isNotEmpty() && role != "model"

    fun label(role: String): String = when (role) {
        "projector" -> "projector"
        "draft" -> "draft head"
        "model" -> "model"
        else -> "not a model"
    }

    /** The models first, the companion files after them, each in the order given. */
    fun <T> split(items: List<T>, role: (T) -> String): Pair<List<T>, List<T>> = items.partition { !isCompanion(role(it)) }

    /** The date part of an ISO timestamp, "" when there is none. */
    fun isoDate(ts: String): String = if (Regex("^\\d{4}-\\d{2}-\\d{2}").containsMatchIn(ts)) ts.take(10) else ""
}
