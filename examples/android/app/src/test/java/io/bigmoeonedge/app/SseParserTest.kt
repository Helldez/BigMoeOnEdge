package io.bigmoeonedge.app

import io.bigmoeonedge.app.engine.SseEvent
import io.bigmoeonedge.app.engine.SseParser
import org.junit.Assert.assertEquals
import org.junit.Test

/** The event-stream rules the server's `/api/events` and chat stream rely on. */
class SseParserTest {
    @Test
    fun namedEventsWithOneDataLine() {
        val p = SseParser()
        val out = p.push("event: state\ndata: {\"a\":1}\n\nevent: token\ndata: {\"b\":2}\n\n")
        assertEquals(listOf(SseEvent("state", "{\"a\":1}"), SseEvent("token", "{\"b\":2}")), out)
    }

    @Test
    fun aChunkMaySplitALineAnywhere() {
        val p = SseParser()
        val out = p.push("data: hel") + p.push("lo\n") + p.push("\n")
        assertEquals(listOf(SseEvent("message", "hello")), out)
    }

    @Test
    fun crlfAcrossChunksIsOneTerminator() {
        val p = SseParser()
        val out = p.push("data: x\r") + p.push("\n\r\n")
        assertEquals(listOf(SseEvent("message", "x")), out)
    }

    @Test
    fun commentsAreKeepAlivesAndDataLinesJoin() {
        val p = SseParser()
        val out = p.push(": ping\n\ndata: a\ndata: b\n\n")
        assertEquals(listOf(SseEvent("message", "a\nb")), out)
    }

    @Test
    fun theDoneSentinelArrivesLikeAnyData() {
        val p = SseParser()
        assertEquals(listOf(SseEvent("message", "[DONE]")), p.push("data: [DONE]\n\n"))
    }

    @Test
    fun aPendingEventWithoutABlankLineIsDroppedAtTheEnd() {
        val p = SseParser()
        assertEquals(emptyList<SseEvent>(), p.push("data: tail"))
        assertEquals(emptyList<SseEvent>(), p.end())
    }
}
