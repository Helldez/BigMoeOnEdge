package io.bigmoeonedge.app

import io.bigmoeonedge.app.chat.ChatItem
import io.bigmoeonedge.app.chat.ConversationCreator
import io.bigmoeonedge.app.chat.ConversationFull
import io.bigmoeonedge.app.chat.ConversationSummary
import io.bigmoeonedge.app.chat.ItemReducer
import io.bigmoeonedge.app.chat.LocalConversations
import io.bigmoeonedge.app.chat.Migration
import io.bigmoeonedge.app.chat.timeMs
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

/** The chat's client-side rules (docs/agent.md): the transcript, Answer again, the migration, the shapes. */
class ChatTest {
    private fun user(id: String, text: String = "q $id") = ChatItem(id, ChatItem.KIND_USER, text = text)
    private fun answer(id: String, text: String = "a $id", streaming: Boolean = false) =
        ChatItem(id, ChatItem.KIND_ASSISTANT, text = text, streaming = streaming)

    // ── the reducer ──

    @Test
    fun anItemIsUpsertedInPlaceAndANewOneAppended() {
        var items = ItemReducer.upsert(emptyList(), user("a", "hi"))
        items = ItemReducer.upsert(items, answer("b", "x", streaming = true))
        items = ItemReducer.upsert(items, ChatItem("c", ChatItem.KIND_NOTICE, text = "n"))
        // b finishes: it changes where it is, the order stays the order of first appearance.
        items = ItemReducer.upsert(items, answer("b", "xy"))
        assertEquals(listOf("a", "b", "c"), items.map { it.id })
        assertEquals("xy", items[1].text)
        assertFalse(items[1].streaming)
    }

    @Test
    fun aDeltaAppendsAndStartsAnItemItHasNotSeen() {
        var items = ItemReducer.delta(emptyList(), "m", "Hel", null)
        assertEquals(1, items.size)
        assertTrue(items[0].streaming)
        assertEquals(ChatItem.KIND_ASSISTANT, items[0].kind)
        items = ItemReducer.delta(items, "m", "lo", "because")
        items = ItemReducer.delta(items, "m", null, " so")
        assertEquals("Hello", items[0].text)
        assertEquals("because so", items[0].reasoning)
        // The whole item lands at the end of the stream: it replaces what the deltas built.
        items = ItemReducer.upsert(items, ChatItem("m", ChatItem.KIND_ASSISTANT, text = "Hello!", streaming = false))
        assertEquals("Hello!", items.single().text)
        assertFalse(items.single().streaming)
    }

    @Test
    fun eventsOfAnotherConversationChangeNothing() {
        val items = listOf(ChatItem("a", ChatItem.KIND_USER, text = "hi"))
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
        val added = ItemReducer.conversation(list, JSONObject().put("id", "elsewhere").put("title", "From the web UI").put("state", "running"))
        assertEquals("elsewhere", added.first().id)
        assertEquals(3, added.size)
    }

    // ── Answer again ──

    @Test
    fun answerAgainSitsOnTheLastWholeAnswerAndResendsTheLastQuestion() {
        val items = listOf(user("1"), answer("2"), user("3", "again?"), answer("4"), ChatItem("5", ChatItem.KIND_NOTICE, text = "slow"))
        // A notice after the answer does not move it; only the last answer carries it.
        assertEquals("4", ItemReducer.answerAgainOn(items))
        val last = ItemReducer.lastUser(items)!!
        assertEquals("3", last.id)
        assertEquals("again?", last.text)
        // The edit route drops the question and what follows it, then sends it again as it was.
        assertEquals(listOf("1", "2"), ItemReducer.before(items, last.id).map { it.id })
        val body = ItemReducer.editBody(last.id, last.text)
        assertEquals("3", body.getString("item"))
        assertEquals("again?", body.getString("text"))
        assertEquals(2, body.length())
    }

    @Test
    fun noAnswerAgainWhileStreamingOrWithoutAQuestion() {
        assertNull(ItemReducer.answerAgainOn(listOf(user("1"), answer("2", streaming = true))))
        // A question waiting for its answer: the earlier answer is not the last one any more.
        assertNull(ItemReducer.answerAgainOn(listOf(user("1"), answer("2"), user("3"))))
        assertNull(ItemReducer.answerAgainOn(listOf(answer("1"))))
        assertNull(ItemReducer.answerAgainOn(emptyList()))
        assertEquals(listOf("1"), ItemReducer.before(listOf(user("1")), "missing").map { it.id })
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
            """{"id":"c1","title":"Fix","state":"running","updated":1761580800,
               "items":[{"id":"1","kind":"user","text":"go"},
                        {"id":"2","kind":"assistant","text":"ok","reasoning":"hm","streaming":true},
                        {"id":"5","kind":"notice","text":"Context nearly full","level":"warn"}]}""",
        )
        val c = ConversationFull.of(o)
        assertEquals(1761580800_000L, c.summary.updated)
        assertTrue(c.summary.busy)
        assertEquals(3, c.items.size)
        assertTrue(c.items[1].streaming)
        assertEquals("hm", c.items[1].reasoning)
        assertEquals("warn", c.items[2].level)
    }

    @Test
    fun aSparseConversationDecodesToDefaults() {
        val c = ConversationSummary.of(JSONObject("""{"id":"x","title":null}"""))
        assertEquals("", c.title)
        assertEquals(ConversationSummary.STATE_IDLE, c.state)
        assertFalse(c.busy)
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
}
