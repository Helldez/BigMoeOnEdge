package io.bigmoeonedge.app

import io.bigmoeonedge.app.agent.AgentItem
import io.bigmoeonedge.app.agent.AgentMode
import io.bigmoeonedge.app.agent.AgentTask
import io.bigmoeonedge.app.agent.Block
import io.bigmoeonedge.app.agent.ConversationCreator
import io.bigmoeonedge.app.agent.ConversationFull
import io.bigmoeonedge.app.agent.ConversationSummary
import io.bigmoeonedge.app.agent.ItemReducer
import io.bigmoeonedge.app.agent.LocalConversations
import io.bigmoeonedge.app.agent.Migration
import io.bigmoeonedge.app.agent.Provider
import io.bigmoeonedge.app.agent.RunSummary
import io.bigmoeonedge.app.agent.Schedule
import io.bigmoeonedge.app.agent.Schedules
import io.bigmoeonedge.app.agent.Skill
import io.bigmoeonedge.app.agent.Workspace
import io.bigmoeonedge.app.agent.routesOf
import io.bigmoeonedge.app.agent.timeMs
import io.bigmoeonedge.app.data.Conversation
import io.bigmoeonedge.app.engine.ChatMessage
import io.bigmoeonedge.app.engine.ChoiceScore
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException

/** The agent's client-side rules (docs/agent.md): the transcript, the summaries, the migration, the shapes. */
class AgentTest {
    private fun tool(id: String, tool: String, state: String = AgentItem.STATE_DONE) =
        AgentItem(id, AgentItem.KIND_TOOL, tool = tool, title = "$tool $id", state = state)

    // ── the reducer ──

    @Test
    fun anItemIsUpsertedInPlaceAndANewOneAppended() {
        val a = AgentItem("a", AgentItem.KIND_USER, text = "hi")
        val b = tool("b", "read_file", AgentItem.STATE_RUNNING)
        var items = ItemReducer.upsert(emptyList(), a)
        items = ItemReducer.upsert(items, b)
        items = ItemReducer.upsert(items, AgentItem("c", AgentItem.KIND_ASSISTANT, text = "x"))
        // b finishes: it changes where it is, the order stays the order of first appearance.
        items = ItemReducer.upsert(items, b.copy(state = AgentItem.STATE_DONE, result = "42 lines"))
        assertEquals(listOf("a", "b", "c"), items.map { it.id })
        assertEquals(AgentItem.STATE_DONE, items[1].state)
        assertEquals("42 lines", items[1].result)
    }

    @Test
    fun aDeltaAppendsAndStartsAnItemItHasNotSeen() {
        var items = ItemReducer.delta(emptyList(), "m", "Hel", null)
        assertEquals(1, items.size)
        assertTrue(items[0].streaming)
        assertEquals(AgentItem.KIND_ASSISTANT, items[0].kind)
        items = ItemReducer.delta(items, "m", "lo", "because")
        items = ItemReducer.delta(items, "m", null, " so")
        assertEquals("Hello", items[0].text)
        assertEquals("because so", items[0].reasoning)
        // The whole item lands at the end of the stream: it replaces what the deltas built.
        items = ItemReducer.upsert(items, AgentItem("m", AgentItem.KIND_ASSISTANT, text = "Hello!", streaming = false))
        assertEquals("Hello!", items.single().text)
        assertFalse(items.single().streaming)
    }

    @Test
    fun eventsOfAnotherConversationChangeNothing() {
        val items = listOf(AgentItem("a", AgentItem.KIND_USER, text = "hi"))
        val other = JSONObject().put("conversation", "other").put("item", JSONObject().put("id", "z").put("kind", "user").put("text", "x"))
        assertEquals(items, ItemReducer.apply("open", items, "item", other))
        assertEquals(items, ItemReducer.apply(null, items, "item", other))
        val mine = JSONObject().put("conversation", "open").put("item", "m").put("text", "Hi")
        assertEquals("Hi", ItemReducer.apply("open", items, "delta", mine).last().text)
        val whole = JSONObject().put("conversation", "open").put("item", JSONObject().put("id", "a").put("kind", "user").put("text", "edited"))
        assertEquals("edited", ItemReducer.apply("open", items, "item", whole).single().text)
    }

    @Test
    fun aConversationEventUpdatesItsRowAndAddsAnUnknownOneOnTop() {
        val list = listOf(ConversationSummary("old", "Old", updated = 1_000), ConversationSummary("b", "B", updated = 2_000))
        val moved = ItemReducer.conversation(list, JSONObject().put("id", "old").put("state", "running").put("updated", 3))
        assertEquals(listOf("old", "b"), moved.map { it.id })
        assertEquals("running", moved[0].state)
        assertEquals("Old", moved[0].title)
        val added = ItemReducer.conversation(list, JSONObject().put("id", "task-run").put("title", "Nightly").put("state", "running"))
        assertEquals("task-run", added.first().id)
        assertEquals(3, added.size)
    }

    // ── the run summary ──

    @Test
    fun aRunOfToolsFoldsUnderItsCounts() {
        val items = listOf(
            AgentItem("u", AgentItem.KIND_USER, text = "fix it"),
            tool("1", "read_file"), tool("2", "read_file"), tool("3", "edit_file"), tool("4", "read_file"), tool("5", "edit_file"),
            AgentItem("a", AgentItem.KIND_ASSISTANT, text = "done"),
        )
        val blocks = RunSummary.blocks(items)
        assertEquals(3, blocks.size)
        val run = blocks[1] as Block.Run
        assertEquals(5, run.items.size)
        assertEquals("Read 3 files, edited 2 files", run.summary)
    }

    @Test
    fun oneToolStaysAloneAndAnApprovalNeverFolds() {
        val single = RunSummary.blocks(listOf(tool("1", "list_files"), AgentItem("a", AgentItem.KIND_ASSISTANT)))
        assertTrue(single[0] is Block.One)
        val waiting = tool("3", "write_file", AgentItem.STATE_WAITING)
        val blocks = RunSummary.blocks(listOf(tool("1", "read_file"), tool("2", "search_files"), waiting))
        assertEquals(2, blocks.size)
        assertEquals("Read 1 file, searched 1 time", (blocks[0] as Block.Run).summary)
        assertEquals(waiting, (blocks[1] as Block.One).item)
    }

    @Test
    fun summaryWordsForEveryTool() {
        assertEquals("Fetched 2 pages, searched the web 1 time", RunSummary.text(listOf(tool("1", "fetch_url"), tool("2", "web_search"), tool("3", "fetch_url"))))
        assertEquals("Used 1 skill, wrote 2 files, used 1 tool", RunSummary.text(listOf(tool("1", "use_skill"), tool("2", "write_file"), tool("3", "write_file"), tool("4", "mystery"))))
    }

    // ── schedules ──

    @Test
    fun schedulesInPlainWords() {
        assertEquals("Every day at 09:00", Schedules.describe(Schedule(Schedule.DAILY, "09:00")))
        assertEquals("Every Mon, Wed at 07:30", Schedules.describe(Schedule(Schedule.WEEKLY, "07:30", weekdays = listOf(3, 1))))
        assertEquals("Every weekday at 08:00", Schedules.describe(Schedule(Schedule.WEEKLY, "08:00", weekdays = listOf(5, 4, 3, 2, 1))))
        assertEquals("Every Sun at 10:00", Schedules.describe(Schedule(Schedule.WEEKLY, "10:00", weekdays = listOf(0))))
        assertEquals("Every 30 minutes", Schedules.describe(Schedule(Schedule.INTERVAL, everyMinutes = 30)))
        assertEquals("Every hour", Schedules.describe(Schedule(Schedule.INTERVAL, everyMinutes = 60)))
        assertEquals("Every 2 hours", Schedules.describe(Schedule(Schedule.INTERVAL, everyMinutes = 120)))
        assertEquals("Once, 27 Oct at 14:00", Schedules.describe(Schedule(Schedule.ONCE, "2026-10-27T14:00")))
    }

    // ── the migration ──

    private class FakeLocal(val convs: MutableMap<String, Conversation>) : LocalConversations {
        override fun ids() = convs.keys.toList()
        override fun load(id: String) = convs[id]
        override fun delete(id: String) {
            convs.remove(id)
        }
    }

    private class FakeServer(val keepsItems: Boolean, val failAfter: Int = Int.MAX_VALUE) : ConversationCreator {
        val created = mutableListOf<JSONObject>()
        val deleted = mutableListOf<String>()
        override fun createConversation(body: JSONObject): JSONObject {
            if (created.size >= failAfter) throw IOException("connection refused")
            created += body
            val o = JSONObject().put("id", "s${created.size}").put("title", body.optString("title"))
            if (keepsItems) o.put("items", body.getJSONArray("items")) else o.put("items", JSONArray())
            return o
        }

        override fun deleteConversation(id: String) {
            deleted += id
        }
    }

    private fun conv(id: String, vararg messages: ChatMessage) =
        Conversation(id, "Title $id", 1, 2, "model", messages.toList())

    @Test
    fun migrationMovesEveryConversationAndDeletesTheLocalCopy() {
        val local = FakeLocal(
            mutableMapOf(
                "a" to conv("a", ChatMessage("user", "hi"), ChatMessage("assistant", "hello", reasoning = "r")),
                "b" to conv("b", ChatMessage("user", "Pick\nA) x\nB) y"), ChatMessage("assistant", "", choices = listOf(ChoiceScore("A", "x", 0.7), ChoiceScore("B", "y", 0.2)), best = 0)),
            ),
        )
        val server = FakeServer(keepsItems = true)
        val r = Migration.run(local, server)
        assertEquals(2, r.moved)
        assertEquals(0, r.kept)
        assertTrue(local.convs.isEmpty())
        val first = server.created.first { it.getString("title") == "Title a" }.getJSONArray("items")
        assertEquals("user", first.getJSONObject(0).getString("kind"))
        assertEquals("hello", first.getJSONObject(1).getString("text"))
        val decision = server.created.first { it.getString("title") == "Title b" }.getJSONArray("items").getJSONObject(1).getString("text")
        assertEquals("A) x: 70% (picked)\nB) y: 20%", decision)
    }

    @Test
    fun aServerThatDropsTheItemsLosesNothing() {
        val local = FakeLocal(mutableMapOf("a" to conv("a", ChatMessage("user", "hi"), ChatMessage("assistant", "hello"))))
        val server = FakeServer(keepsItems = false)
        val r = Migration.run(local, server)
        assertEquals(0, r.moved)
        assertEquals(1, r.kept)
        assertTrue("the phone keeps its copy", local.convs.containsKey("a"))
        assertEquals(listOf("s1"), server.deleted)
    }

    @Test
    fun aServerThatIsDownIsTriedAgainLater() {
        val local = FakeLocal(mutableMapOf("a" to conv("a", ChatMessage("user", "hi")), "b" to conv("b", ChatMessage("user", "yo"))))
        val r = Migration.run(local, FakeServer(keepsItems = true, failAfter = 0))
        assertEquals(0, r.moved)
        assertTrue(r.error!!.contains("refused"))
        assertEquals(2, local.convs.size)
    }

    @Test
    fun anEmptyConversationIsDroppedNotSent() {
        val local = FakeLocal(mutableMapOf("e" to conv("e")))
        val server = FakeServer(keepsItems = true)
        Migration.run(local, server)
        assertTrue(local.convs.isEmpty())
        assertTrue(server.created.isEmpty())
    }

    // ── the contract's shapes ──

    @Test
    fun conversationAndItemsDecode() {
        val o = JSONObject(
            """{"id":"c1","title":"Fix","workspace":"w1","mode":"full","route":"p/m","state":"waiting","updated":1761580800,"task":null,
               "items":[{"id":"1","kind":"user","text":"go"},
                        {"id":"2","kind":"assistant","text":"ok","reasoning":"hm","streaming":true},
                        {"id":"3","kind":"tool","tool":"edit_file","title":"Edit a.c","args":{"path":"a.c"},"state":"waiting","diff":"-a\n+b","file":"a.c"},
                        {"id":"4","kind":"helper","title":"Look up","state":"done","summary":"found","conversation":"c2"},
                        {"id":"5","kind":"notice","text":"No web","level":"warn"}]}""",
        )
        val c = ConversationFull.of(o)
        assertEquals(AgentMode.FULL, c.summary.mode)
        assertEquals("p/m", c.summary.route)
        assertEquals(1761580800_000L, c.summary.updated)
        assertEquals("", c.summary.task)
        assertTrue(c.summary.busy)
        assertEquals(5, c.items.size)
        assertTrue(c.items[1].streaming)
        assertTrue(c.items[2].awaitsApproval)
        assertEquals("a.c", JSONObject(c.items[2].args).getString("path"))
        assertEquals("c2", c.items[3].conversation)
        assertEquals("warn", c.items[4].level)
    }

    @Test
    fun aSparseConversationDecodesToDefaults() {
        val c = ConversationSummary.of(JSONObject("""{"id":"x"}"""))
        assertEquals(AgentMode.ASK, c.mode)
        assertEquals(ConversationSummary.ROUTE_LOCAL, c.route)
        assertEquals(ConversationSummary.STATE_IDLE, c.state)
        assertEquals(0L, c.updated)
    }

    @Test
    fun timesInEveryEncoding() {
        assertEquals(1_000_000_000_000L, timeMs(1_000_000_000L))
        assertEquals(1_000_000_000_000L, timeMs(1_000_000_000_000L))
        assertEquals(1_000_000_000_000L, timeMs("2001-09-09T01:46:40Z"))
        assertEquals(0L, timeMs(null))
        assertEquals(0L, timeMs("soon"))
    }

    @Test
    fun workspacesSkillsTasksProvidersDecode() {
        val w = Workspace.list(JSONArray("""[{"id":"w1","name":"Notes","path":"/data/notes"}]"""))
        assertEquals(Workspace("w1", "Notes", "/data/notes"), w.single())

        val s = Skill.list(JSONArray("""[{"name":"release-notes","title":"Release notes","description":"d","source":"user","enabled":false,"body":"b"},{"name":"x","source":"bundled"}]"""))
        assertTrue(s[0].editable)
        assertFalse(s[0].enabled)
        assertFalse(s[1].editable)
        assertTrue(s[1].enabled)
        assertTrue(Skill.validName("release-notes"))
        assertFalse(Skill.validName("Release Notes"))
        assertFalse(Skill.validName("-x"))

        val t = AgentTask.of(
            JSONObject(
                """{"id":"t1","title":"Nightly","prompt":"sum up","workspace":"w1","route":"local",
                   "schedule":{"kind":"weekly","at":"07:30","every_minutes":0,"weekdays":[1,3]},"enabled":true,
                   "last_run":{"at":"2026-10-26T07:30:00Z","conversation":"c9","outcome":"done"},"next_run":1761636600}""",
            ),
        )
        assertEquals(AgentMode.READ_ONLY, t.mode) // a task runs read-only unless one was picked
        assertEquals(listOf(1, 3), t.schedule.weekdays)
        assertEquals("c9", t.lastRun!!.conversation)
        assertEquals(1761636600_000L, t.nextRun)
        val body = t.toJson()
        assertEquals("weekly", body.getJSONObject("schedule").getString("kind"))
        assertEquals("read_only", body.getString("mode"))
        assertFalse(body.has("id"))
        assertTrue(AgentTask.blank().toJson().isNull("workspace"))

        val p = Provider.list(JSONArray("""[{"id":"p1","name":"Cloud","base_url":"https://x/v1","models":["a","b"],"has_key":true}]"""))
        assertTrue(p.single().hasKey)
        assertEquals(listOf("local", "p1/a", "p1/b"), routesOf(p).map { it.wire })
        assertEquals("Cloud · a", routesOf(p)[1].label)
        assertNull(p.single().models.firstOrNull { it.isEmpty() })
    }
}
