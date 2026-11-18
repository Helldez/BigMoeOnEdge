package io.bigmoeonedge.app

import io.bigmoeonedge.app.engine.Api
import io.bigmoeonedge.app.engine.ApiException
import io.bigmoeonedge.app.engine.ChatMessage
import io.bigmoeonedge.app.engine.ChatProtocol
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The chat-completions protocol as the app speaks it: what goes out, what each chunk means. */
class ChatProtocolTest {
    @Test
    fun theWholeConversationGoesOutWithReasoningOnAssistantTurns() {
        val body = ChatProtocol.requestBody(
            listOf(
                ChatMessage("user", "hi"),
                ChatMessage("assistant", "hello", reasoning = "greet back", metrics = "1 tok/s"),
                ChatMessage("user", "and?"),
            ),
        )
        val msgs = body.getJSONArray("messages")
        assertEquals(3, msgs.length())
        assertEquals("hi", msgs.getJSONObject(0).getString("content"))
        assertEquals("greet back", msgs.getJSONObject(1).getString("reasoning_content"))
        assertTrue(!msgs.getJSONObject(0).has("reasoning_content"))
        assertTrue(!msgs.getJSONObject(1).has("metrics"))
        assertTrue(body.getBoolean("stream"))
    }

    @Test
    fun deltasResetsAndTheLastChunk() {
        val c1 = ChatProtocol.parseChunk("""{"choices":[{"delta":{"role":"assistant","content":"Hel"},"finish_reason":null}]}""")!!
        assertEquals("Hel", c1.content)
        assertNull(c1.reasoning)
        assertTrue(!c1.reset)
        assertNull(c1.finish)

        val c2 = ChatProtocol.parseChunk("""{"choices":[{"delta":{"content":"Hello","reasoning_content":"thinking"},"finish_reason":null}],"bmoe":{"reset":true}}""")!!
        assertTrue(c2.reset)
        assertEquals("Hello", c2.content)
        assertEquals("thinking", c2.reasoning)

        val c3 = ChatProtocol.parseChunk("""{"choices":[{"delta":{},"finish_reason":"stop"}],"bmoe":{"summary":{"tokens":3,"tok_s":2.5}}}""")!!
        assertEquals("stop", c3.finish)
        assertEquals(3, c3.summary!!.getInt("tokens"))
    }

    @Test
    fun doneAndErrorsAndUsageOnlyChunks() {
        assertNull(ChatProtocol.parseChunk("[DONE]"))
        assertNull(ChatProtocol.parseChunk(""))
        assertEquals("boom", ChatProtocol.parseChunk("""{"error":{"message":"boom","code":"failed"}}""")!!.error)
        val full = ChatProtocol.parseChunk("""{"error":{"message":"any wording","code":"context_full"}}""")!!
        assertEquals(ApiException.CODE_CONTEXT_FULL, full.errorCode)
        assertEquals("", ChatProtocol.parseChunk("""{"error":{"message":"no code"}}""")!!.errorCode)
        assertEquals("context_full", Api.errorCode("""{"error":"x","code":"context_full"}"""))
        assertEquals("", Api.errorCode("not json"))
        val usage = ChatProtocol.parseChunk("""{"choices":[],"usage":{"prompt_tokens":1}}""")!!
        assertNull(usage.content)
        assertNull(usage.finish)
    }
}
