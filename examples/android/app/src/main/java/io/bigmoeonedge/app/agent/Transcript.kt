package io.bigmoeonedge.app.agent

import org.json.JSONObject
import java.time.DayOfWeek
import java.time.LocalDateTime
import java.time.LocalTime
import java.time.format.DateTimeFormatter
import java.time.format.TextStyle
import java.util.Locale

/**
 * The open conversation as the events build it: pure, so the rules the transcript depends on
 * (where an item lands, what a delta adds) are tested without a server.
 */
object ItemReducer {
    /** An `item` event: the item whole. It replaces the one with its id in place, or is appended. */
    fun upsert(items: List<AgentItem>, item: AgentItem): List<AgentItem> {
        val i = items.indexOfFirst { it.id == item.id }
        if (i < 0) return items + item
        return items.toMutableList().also { it[i] = item }
    }

    /**
     * A `delta` event: what a streaming assistant item adds. A delta for an item not seen yet (the
     * stream was joined mid-answer, or its `item` event is still on the way) starts it.
     */
    fun delta(items: List<AgentItem>, id: String, text: String?, reasoning: String?): List<AgentItem> {
        val i = items.indexOfFirst { it.id == id }
        if (i < 0) {
            return items + AgentItem(id, AgentItem.KIND_ASSISTANT, text = text.orEmpty(), reasoning = reasoning.orEmpty(), streaming = true)
        }
        val cur = items[i]
        val next = cur.copy(text = cur.text + text.orEmpty(), reasoning = cur.reasoning + reasoning.orEmpty())
        return items.toMutableList().also { it[i] = next }
    }

    /** An event's effect on the open conversation [openId]: another conversation's events change nothing here. */
    fun apply(openId: String?, items: List<AgentItem>, event: String, data: JSONObject): List<AgentItem> {
        if (openId == null || data.optString("conversation") != openId) return items
        return when (event) {
            "item" -> data.optJSONObject("item")?.let { upsert(items, AgentItem.of(it)) } ?: items
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
     * yet (a task's run, a conversation another client opened) goes to the top. Newest first.
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
}

/** What the transcript draws: an item on its own, or a run of tool calls folded under one line. */
sealed class Block {
    data class One(val item: AgentItem) : Block()
    data class Run(val items: List<AgentItem>, val summary: String) : Block() {
        val running get() = items.any { it.state == AgentItem.STATE_RUNNING }
        val failed get() = items.count { it.state == AgentItem.STATE_ERROR || it.state == AgentItem.STATE_DENIED }
    }
}

object RunSummary {
    /**
     * The items as blocks: two or more tool calls in a row fold under their summary; one waiting
     * for an approval never folds, since it is the card a person has to answer.
     */
    fun blocks(items: List<AgentItem>): List<Block> {
        val out = mutableListOf<Block>()
        val run = mutableListOf<AgentItem>()
        fun flush() {
            when {
                run.size >= 2 -> out += Block.Run(run.toList(), text(run))
                run.size == 1 -> out += Block.One(run[0])
            }
            run.clear()
        }
        for (it in items) {
            if (it.isTool && !it.awaitsApproval) {
                run += it
            } else {
                flush()
                out += Block.One(it)
            }
        }
        flush()
        return out
    }

    /** "Read 3 files, edited 2 files": each tool's count, in the order they first ran. */
    fun text(items: List<AgentItem>): String {
        val counts = LinkedHashMap<String, Int>()
        for (it in items.filter { it.isTool }) counts[it.tool] = (counts[it.tool] ?: 0) + 1
        val parts = counts.map { (tool, n) -> phrase(tool, n) }
        val joined = parts.joinToString(", ")
        return joined.replaceFirstChar { it.uppercase() }
    }

    private fun phrase(tool: String, n: Int): String {
        fun plural(one: String, many: String) = if (n == 1) one else many
        return when (tool) {
            "read_file" -> "read $n ${plural("file", "files")}"
            "list_files" -> "listed $n ${plural("folder", "folders")}"
            "search_files" -> "searched $n ${plural("time", "times")}"
            "write_file" -> "wrote $n ${plural("file", "files")}"
            "edit_file" -> "edited $n ${plural("file", "files")}"
            "fetch_url" -> "fetched $n ${plural("page", "pages")}"
            "web_search" -> "searched the web $n ${plural("time", "times")}"
            "use_skill" -> "used $n ${plural("skill", "skills")}"
            "run_command" -> "ran $n ${plural("command", "commands")}"
            "delegate" -> "handed off $n ${plural("task", "tasks")}"
            else -> "used $n ${plural("tool", "tools")}"
        }
    }
}

object Schedules {
    private val dayMonth = DateTimeFormatter.ofPattern("d MMM", Locale.ENGLISH)

    /** A schedule in plain words: "Every day at 09:00", "Every Mon, Wed at 09:00", "Once, 27 Oct at 14:00". */
    fun describe(s: Schedule): String = when (s.kind) {
        Schedule.ONCE -> runCatching {
            val t = LocalDateTime.parse(s.at)
            "Once, ${t.format(dayMonth)} at ${hhmm(t.toLocalTime())}"
        }.getOrElse { if (s.at.isEmpty()) "Once" else "Once, ${s.at}" }
        Schedule.INTERVAL -> interval(s.everyMinutes)
        Schedule.DAILY -> "Every day at ${time(s.at)}"
        Schedule.WEEKLY -> {
            val days = s.weekdays.distinct().sortedBy { (it + 6) % 7 } // Monday first, as a week reads
            when {
                days.isEmpty() -> "Weekly, no day picked"
                days.size == 7 -> "Every day at ${time(s.at)}"
                days == listOf(1, 2, 3, 4, 5) -> "Every weekday at ${time(s.at)}"
                else -> "Every ${days.joinToString(", ") { dayName(it) }} at ${time(s.at)}"
            }
        }
        else -> s.kind
    }

    fun interval(minutes: Int): String = when {
        minutes <= 0 -> "Repeats, no interval set"
        minutes == 60 -> "Every hour"
        minutes % 1440 == 0 -> (minutes / 1440).let { if (it == 1) "Every day" else "Every $it days" }
        minutes % 60 == 0 -> "Every ${minutes / 60} hours"
        minutes == 1 -> "Every minute"
        else -> "Every $minutes minutes"
    }

    /** 0 = Sunday … 6 = Saturday, as the schedule numbers them. */
    fun dayName(d: Int): String = DayOfWeek.of(if (d % 7 == 0) 7 else d % 7).getDisplayName(TextStyle.SHORT, Locale.ENGLISH)

    private fun time(at: String): String = runCatching { hhmm(LocalTime.parse(at)) }.getOrElse { at.ifEmpty { "--:--" } }
    private fun hhmm(t: LocalTime) = "%02d:%02d".format(t.hour, t.minute)
}
