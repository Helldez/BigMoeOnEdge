package io.bigmoeonedge.app

import io.bigmoeonedge.app.engine.SystemPrompt
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The system prompt is shown as the server sent it, with its cost when a model counted it. */
class SystemPromptTest {
    @Test
    fun aCountedPromptCarriesItsTokens() {
        val p = SystemPrompt.of(JSONObject("""{"text":"You are a coding agent.","tokens":153,"external":false}"""))
        assertEquals("You are a coding agent.", p.text)
        assertEquals(153, p.tokens)
        assertFalse(p.external)
    }

    @Test
    fun noModelMeansNoCount() {
        assertNull(SystemPrompt.of(JSONObject("""{"text":"x","tokens":null,"external":false}""")).tokens)
    }

    @Test
    fun anotherAgentProgramWritesItsOwn() {
        val p = SystemPrompt.of(JSONObject("""{"text":"","tokens":null,"external":true}"""))
        assertTrue(p.external)
        assertEquals("", p.text)
    }
}
