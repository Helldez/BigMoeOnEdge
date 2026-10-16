package io.bigmoeonedge.app

import io.bigmoeonedge.app.data.Conversation
import io.bigmoeonedge.app.data.ConversationStore
import io.bigmoeonedge.app.engine.ChatMessage
import io.bigmoeonedge.app.engine.ParamSpec
import io.bigmoeonedge.app.engine.paramValueFromText
import io.bigmoeonedge.app.engine.paramValueText
import org.json.JSONArray
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files

/** The parameter table as the settings screen reads it, and the conversation files. */
class ParamsAndConversationsTest {
    private val table = JSONArray(
        """[
          {"key":"cache-mb","label":"Expert cache","type":"int","group":"cache","group_label":"Expert cache","level":"basic",
           "scope":"session","help":"h","flag":"--cache-mb","min":0,"max":2147483647,"unit":"MiB","accepts_auto":true,"default":"auto"},
          {"key":"overlap","label":"Overlap","type":"bool","group":"cache","group_label":"Expert cache","level":"advanced","scope":"session","help":"","flag":"--overlap","default":true},
          {"key":"dense-weights","label":"Dense","type":"choice","group":"dense","group_label":"Dense weights","level":"basic","scope":"session","help":"",
           "flag":"--dense-weights","choices":[{"value":"anon","label":"Anon"},{"value":"mmap","label":"Mmap"}],"default":"anon"}
        ]""",
    )

    @Test
    fun groupsKeepTheTablesOrderAndLevelsFilter() {
        val specs = ParamSpec.parse(table)
        val groups = ParamSpec.grouped(specs)
        assertEquals(listOf("Expert cache", "Dense weights"), groups.map { it.first })
        assertEquals(listOf("cache-mb", "overlap"), groups[0].second.map { it.key })
        assertTrue(ParamSpec.visible("basic", "basic"))
        assertTrue(!ParamSpec.visible("advanced", "basic"))
        assertTrue(ParamSpec.visible("advanced", "experimental"))
        assertEquals(listOf("anon" to "Anon", "mmap" to "Mmap"), specs[2].choices)
        assertEquals("MiB", specs[0].unit)
        assertTrue(specs[0].acceptsAuto)
    }

    @Test
    fun fieldTextBecomesTheWireValueOfTheRowsType() {
        val specs = ParamSpec.parse(table)
        assertEquals(2000L, paramValueFromText(specs[0], " 2000 "))
        assertEquals("auto", paramValueFromText(specs[0], "auto"))
        assertNull(paramValueFromText(specs[0], "lots"))
        assertEquals(true, paramValueFromText(specs[1], "true"))
        assertNull(paramValueFromText(specs[1], "yes"))
        assertEquals("mmap", paramValueFromText(specs[2], "mmap"))
        assertEquals("4", paramValueText(4.0))
        assertEquals("0.15", paramValueText(0.15))
        assertEquals("auto", paramValueText("auto"))
    }

    @Test
    fun conversationsSurviveARoundTripThroughTheirFiles() {
        val dir = Files.createTempDirectory("conv").toFile()
        val store = ConversationStore(dir)
        val c = Conversation.fresh().copy(
            title = Conversation.titleFor("What is a mixture of experts?\nsecond line"),
            messages = listOf(ChatMessage("user", "What is a mixture of experts?"), ChatMessage("assistant", "A model…", "think", "3 tok/s")),
            model = "m.gguf",
        )
        store.save(c)
        assertEquals(c, store.load(c.id))
        val list = store.list()
        assertEquals(1, list.size)
        assertEquals("What is a mixture of experts?", list[0].title)
        assertEquals(2, list[0].messageCount)
        store.delete(c.id)
        assertNull(store.load(c.id))
        assertEquals(0, store.list().size)
    }

    @Test
    fun titlesAreTheFirstLineCutToFit() {
        assertEquals("New chat", Conversation.titleFor("   \n  "))
        val long = "a".repeat(80)
        assertEquals(48, Conversation.titleFor(long).length)
        assertTrue(Conversation.titleFor(long).endsWith("…"))
    }
}
