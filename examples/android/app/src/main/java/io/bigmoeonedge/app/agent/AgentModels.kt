package io.bigmoeonedge.app.agent

import org.json.JSONArray
import org.json.JSONObject
import java.time.Instant
import java.time.LocalDateTime
import java.time.OffsetDateTime
import java.time.ZoneId

/**
 * The agent's shapes as the server sends them (docs/agent.md). Decoded tolerantly: a field the
 * server leaves out reads as its empty value, so a server a little older or newer than the app
 * still draws a screen instead of failing a parse.
 */

/** The modes a conversation can run in, with the words the screens show. */
enum class AgentMode(val wire: String, val label: String, val help: String) {
    ASK("ask", "Ask first", "Reads freely, asks before it changes a file"),
    READ_ONLY("read_only", "Read only", "Reads, never changes anything"),
    FULL("full", "Full access", "Reads and changes files without asking");

    companion object {
        fun of(wire: String?): AgentMode = values().firstOrNull { it.wire == wire } ?: ASK
    }
}

/** One entry of a conversation, upserted by [id]. Every kind's fields; the others stay empty. */
data class AgentItem(
    val id: String,
    val kind: String,
    val text: String = "",
    val reasoning: String = "",
    val streaming: Boolean = false,
    val tool: String = "",
    val title: String = "",
    /** The tool's arguments as the server sent them (a JSON object's text), for the card's detail. */
    val args: String = "",
    val state: String = "",
    val result: String = "",
    val diff: String = "",
    val file: String = "",
    val summary: String = "",
    val conversation: String = "",
    val level: String = "",
) {
    val isTool get() = kind == KIND_TOOL

    /** A change the agent asked to make and nobody has answered yet. */
    val awaitsApproval get() = kind == KIND_TOOL && state == STATE_WAITING

    companion object {
        const val KIND_USER = "user"
        const val KIND_ASSISTANT = "assistant"
        const val KIND_TOOL = "tool"
        const val KIND_HELPER = "helper"
        const val KIND_NOTICE = "notice"

        const val STATE_RUNNING = "running"
        const val STATE_WAITING = "waiting"
        const val STATE_DONE = "done"
        const val STATE_DENIED = "denied"
        const val STATE_ERROR = "error"

        fun of(o: JSONObject): AgentItem = AgentItem(
            id = o.optString("id"),
            kind = o.optString("kind"),
            text = o.str("text"),
            reasoning = o.str("reasoning"),
            streaming = o.optBoolean("streaming", false),
            tool = o.str("tool"),
            title = o.str("title"),
            args = o.optJSONObject("args")?.toString() ?: "",
            state = o.str("state"),
            result = o.str("result"),
            diff = o.str("diff"),
            file = o.str("file"),
            summary = o.str("summary"),
            conversation = o.str("conversation"),
            level = o.str("level"),
        )

        fun list(a: JSONArray?): List<AgentItem> =
            a?.let { arr -> (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.let(::of) } }.orEmpty()
    }
}

/** A row of the conversation list. [state]: idle, running, waiting (an approval) or error. */
data class ConversationSummary(
    val id: String,
    val title: String,
    val workspace: String = "",
    val mode: AgentMode = AgentMode.ASK,
    val route: String = ROUTE_LOCAL,
    val state: String = STATE_IDLE,
    /** Milliseconds since the epoch; 0 when the server sent none. */
    val updated: Long = 0,
    /** The task that ran it, when a schedule did. */
    val task: String = "",
) {
    val busy get() = state == STATE_RUNNING || state == STATE_WAITING

    companion object {
        const val ROUTE_LOCAL = "local"
        const val STATE_IDLE = "idle"
        const val STATE_RUNNING = "running"
        const val STATE_WAITING = "waiting"
        const val STATE_ERROR = "error"

        fun of(o: JSONObject): ConversationSummary = ConversationSummary(
            id = o.optString("id"),
            title = o.str("title"),
            workspace = o.str("workspace"),
            mode = AgentMode.of(o.str("mode")),
            route = o.str("route").ifEmpty { ROUTE_LOCAL },
            state = o.str("state").ifEmpty { STATE_IDLE },
            updated = timeMs(o.opt("updated")),
            task = o.str("task"),
        )

        fun list(a: JSONArray?): List<ConversationSummary> =
            a?.let { arr -> (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.let(::of) } }.orEmpty()
    }
}

/** A conversation with its items: `GET /api/conversations/<id>`. */
data class ConversationFull(val summary: ConversationSummary, val items: List<AgentItem>) {
    companion object {
        fun of(o: JSONObject) = ConversationFull(ConversationSummary.of(o), AgentItem.list(o.optJSONArray("items")))
    }
}

/** A folder a person added: file tools work inside it and nowhere else. */
data class Workspace(val id: String, val name: String, val path: String) {
    companion object {
        fun of(o: JSONObject) = Workspace(o.optString("id"), o.str("name"), o.str("path"))
        fun list(a: JSONArray?): List<Workspace> =
            a?.let { arr -> (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.let(::of) } }.orEmpty()
    }
}

/** A skill: [source] is bundled, user (written in the app) or workspace. */
data class Skill(
    val name: String,
    val title: String,
    val description: String,
    val source: String,
    val enabled: Boolean,
    val body: String,
) {
    /** Only a person's own skill can be edited or deleted; the others can only be switched. */
    val editable get() = source == SOURCE_USER

    companion object {
        const val SOURCE_USER = "user"

        fun of(o: JSONObject) = Skill(
            o.optString("name"), o.str("title"), o.str("description"), o.str("source"),
            o.optBoolean("enabled", true), o.str("body"),
        )

        fun list(a: JSONArray?): List<Skill> =
            a?.let { arr -> (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.let(::of) } }.orEmpty()

        /** What the server takes as a skill's name: kebab-case, lower-case letters and digits. */
        fun validName(name: String): Boolean = Regex("^[a-z0-9]+(-[a-z0-9]+)*$").matches(name)
    }
}

/**
 * When a task runs. [at] is "YYYY-MM-DDTHH:MM" (local time) for once and "HH:MM" for daily and
 * weekly; [weekdays] are 0-6 with 0 = Sunday. Both are this app's reading of the contract, which
 * names the fields and not their format.
 */
data class Schedule(
    val kind: String,
    val at: String = "",
    val everyMinutes: Int = 0,
    val weekdays: List<Int> = emptyList(),
) {
    fun toJson(): JSONObject = JSONObject().put("kind", kind).put("at", at)
        .put("every_minutes", everyMinutes).put("weekdays", JSONArray(weekdays))

    companion object {
        const val ONCE = "once"
        const val INTERVAL = "interval"
        const val DAILY = "daily"
        const val WEEKLY = "weekly"
        val KINDS = listOf(ONCE, INTERVAL, DAILY, WEEKLY)

        fun of(o: JSONObject?): Schedule {
            if (o == null) return Schedule(DAILY, "09:00")
            val days = o.optJSONArray("weekdays")
            return Schedule(
                kind = o.str("kind").ifEmpty { DAILY },
                at = o.str("at"),
                everyMinutes = o.optInt("every_minutes", 0),
                weekdays = days?.let { a -> (0 until a.length()).map { a.optInt(it) } }.orEmpty(),
            )
        }
    }
}

data class LastRun(val at: Long, val conversation: String, val outcome: String)

/** A prompt that runs as a conversation on a schedule. */
data class AgentTask(
    val id: String,
    val title: String,
    val prompt: String,
    val workspace: String,
    val mode: AgentMode,
    val route: String,
    val schedule: Schedule,
    val enabled: Boolean,
    val lastRun: LastRun?,
    /** Milliseconds since the epoch, or 0. */
    val nextRun: Long,
) {
    /** The body of a create or an update: what a person can set, nothing the server keeps. */
    fun toJson(): JSONObject = JSONObject().put("title", title).put("prompt", prompt)
        .put("workspace", workspace.ifEmpty { JSONObject.NULL }).put("mode", mode.wire).put("route", route)
        .put("schedule", schedule.toJson()).put("enabled", enabled)

    companion object {
        fun of(o: JSONObject): AgentTask {
            val lr = o.optJSONObject("last_run")
            return AgentTask(
                id = o.optString("id"),
                title = o.str("title"),
                prompt = o.str("prompt"),
                workspace = o.str("workspace"),
                // A task runs read-only unless the person picked another mode (docs/agent.md).
                mode = AgentMode.of(o.str("mode").ifEmpty { AgentMode.READ_ONLY.wire }),
                route = o.str("route").ifEmpty { ConversationSummary.ROUTE_LOCAL },
                schedule = Schedule.of(o.optJSONObject("schedule")),
                enabled = o.optBoolean("enabled", true),
                lastRun = lr?.let { LastRun(timeMs(it.opt("at")), it.str("conversation"), it.str("outcome")) },
                nextRun = timeMs(o.opt("next_run")),
            )
        }

        fun list(a: JSONArray?): List<AgentTask> =
            a?.let { arr -> (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.let(::of) } }.orEmpty()

        fun blank() = AgentTask(
            "", "", "", "", AgentMode.READ_ONLY, ConversationSummary.ROUTE_LOCAL,
            Schedule(Schedule.DAILY, "09:00"), true, null, 0,
        )
    }
}

/** A saved OpenAI-compatible service. The key is never read back: only whether one is saved. */
data class Provider(val id: String, val name: String, val baseUrl: String, val models: List<String>, val hasKey: Boolean) {
    companion object {
        fun of(o: JSONObject): Provider {
            val m = o.optJSONArray("models")
            return Provider(
                o.optString("id"), o.str("name"), o.str("base_url"),
                m?.let { a -> (0 until a.length()).map { a.optString(it) }.filter { it.isNotEmpty() } }.orEmpty(),
                o.optBoolean("has_key", false),
            )
        }

        fun list(a: JSONArray?): List<Provider> =
            a?.let { arr -> (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.let(::of) } }.orEmpty()
    }
}

/** A model route a conversation can take, with the words the menu shows. */
data class Route(val wire: String, val label: String)

/** The routes on offer: the loaded model first, then every provider's models. */
fun routesOf(providers: List<Provider>, localLabel: String = "This phone"): List<Route> =
    listOf(Route(ConversationSummary.ROUTE_LOCAL, localLabel)) +
        providers.flatMap { p -> p.models.map { m -> Route("${p.id}/$m", "${p.name.ifEmpty { p.id }} · $m") } }

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
