package io.bigmoeonedge.app

import io.bigmoeonedge.app.chat.ConversationStatus
import io.bigmoeonedge.app.chat.ConversationStatus.Facts
import io.bigmoeonedge.app.chat.ConversationStatus.Mood
import io.bigmoeonedge.app.chat.TurnFigures
import io.bigmoeonedge.app.chat.Words
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** The line under a finished answer, the figures it opens to, and the conversation's status line. */
class TurnFiguresTest {
    private fun summary() = JSONObject(
        """{"tok_s":12.43,"tokens":42,"prefill_s":0.9,"prefill_tps":140.0,"n_prompt":126,"n_past":900,"cache_hit_pct":72.4,
           "flash_wait_s_tok":0.012,"mtp_decodes":0,"mtp_drafted":0}""",
    )

    @Test
    fun theLineIsTgThenPp() {
        val s = summary()
        assertEquals("tg 12.4 tok/s · pp 140 tok/s", TurnFigures.line(s))
        // Without the engine's rate, the tokens read over the time it took.
        s.remove("prefill_tps")
        assertEquals("tg 12.4 tok/s · pp 140 tok/s", TurnFigures.line(s))
        // Unknown: pp is left out rather than shown as 0.
        s.remove("prefill_s")
        assertEquals("tg 12.4 tok/s", TurnFigures.line(s))
        assertEquals("tg 11.1 tok/s", TurnFigures.live(11.06))
    }

    /** The web's fixture (ui/src/lib/turnMetrics.test.ts), so the two UIs are checked on the same numbers. */
    private fun web() = JSONObject(
        """{"cancelled":false,"tokens":42,"tok_s":10,"prefill_s":1.25,"prefill_tps":13.6,"n_prompt":17,"n_past":900,
           "cache_hit_pct":81.4,"compute_s_tok":0.07,"flash_wait_s_tok":0.015,"mgmt_s_tok":0.002,"ttft_s":1.25}""",
    )

    @Test
    fun aTokensTimeSplitsIntoWholePercentagesThatAddUpTo100() {
        val t = TurnFigures.timeShares(web())
        assertEquals(listOf("compute", "disk", "mgmt", "other"), t.map { it.key })
        assertEquals(100, t.sumOf { it.pct })
        assertEquals(70, t.first { it.key == "compute" }.pct)
        val odd = web().put("tok_s", 3).put("compute_s_tok", 0.111).put("flash_wait_s_tok", 0.111).put("mgmt_s_tok", 0.111)
        assertEquals(100, TurnFigures.timeShares(odd).sumOf { it.pct })
        assertEquals(emptyList<Any>(), TurnFigures.timeShares(web().put("tok_s", 0)))
        // A part that rounds to nothing is left out.
        assertEquals(listOf("compute", "other"), TurnFigures.timeShares(web().put("flash_wait_s_tok", 0.0).put("mgmt_s_tok", 0.0)).map { it.key })
        assertEquals(listOf(34, 33, 33), TurnFigures.largestRemainder(listOf(1.0, 1.0, 1.0)))
    }

    @Test
    fun theFiguresSayTheTimeSplitOnceAndMemoryAsACount() {
        val f = TurnFigures.of(web(), 16384, 13.9)
        assertEquals(listOf("speed", "time", "cache", "prompt", "context"), f.map { it.id })
        val by = f.associateBy { it.id }
        assertEquals("10.0 tok/s, plan expected 13.9", by.getValue("speed").value)
        assertEquals("computing 70% · waiting for the disk 15% · managing the cache 2% · other 13%", by.getValue("time").value)
        assertEquals("81 of every 100 parts", by.getValue("cache").value)
        assertEquals("900 of 16,384 tokens", by.getValue("context").value)
        assertEquals("13.6 tok/s, 17 tokens in 1.3 s", by.getValue("prompt").value)
        // No figure but the time split is a percentage.
        assertEquals(emptyList<Any>(), f.filter { it.id != "time" && it.value.contains('%') })
        f.forEach { assertTrue(it.hint.isNotEmpty()) }
        assertEquals(Words.TIME_HINT, by.getValue("time").hint)
    }

    @Test
    fun figuresLeftOutWhenUnknown() {
        val f = TurnFigures.of(web().put("cache_hit_pct", -1), 0, null)
        assertEquals("10.0 tok/s", f[0].value)
        assertEquals(listOf("speed", "time", "prompt"), f.map { it.id })
        assertEquals("stopped", TurnFigures.of(web().put("cancelled", true), 0, null).last().id)
        val s = JSONObject("""{"tok_s":3.0,"cache_hit_pct":-1,"prefill_s":0,"n_past":10,"cancelled":true,"mtp_decodes":4,"mtp_drafted":8,"mtp_accepted":6}""")
        val g = TurnFigures.of(s, 0, 0.0)
        assertEquals(listOf("speed", "time", "drafts", "stopped"), g.map { it.id })
        assertEquals("3.0 tok/s", g[0].value)
        assertEquals("other 100%", g[1].value)
        assertEquals("6 of 8 guesses", g[2].value)
        assertEquals("", g[3].value)
    }

    private val ready = Facts(serverUp = true, infoKnown = true, engineState = "ready")

    @Test
    fun theStatusLineSaysWhatTheEngineDoes() {
        fun say(f: Facts) = ConversationStatus.of(f)
        assertEquals(ConversationStatus(Words.UNREACHABLE, Mood.SLEEP), say(Facts(serverUp = false, infoKnown = true, engineState = "ready")))
        assertEquals(Words.CONNECTING, say(Facts(serverUp = true, infoKnown = false, engineState = "")).text)
        assertEquals(Words.MEASURING, say(ready.copy(engineState = "planning")).text)
        assertEquals(Words.LOADING, say(ready.copy(engineState = "loading")).text)
        assertEquals(ConversationStatus(Words.LOAD_ERROR, Mood.ALERT), say(ready.copy(engineState = "error")))
        assertEquals(Words.NO_MODEL, say(ready.copy(engineState = "empty")).text)
        assertEquals(ConversationStatus(Words.READY, Mood.IDLE), say(ready))
    }

    @Test
    fun theStatusLineFollowsTheConversation() {
        fun say(f: Facts) = ConversationStatus.of(f).text
        assertEquals(Words.THINKING, say(ready.copy(running = true)))
        assertEquals(Words.WRITING, say(ready.copy(running = true, writing = true, lastIsAnswer = true)))
        assertEquals(Words.DISK, say(ready.copy(running = true, writing = true, flashWaitMs = 40.0, wallMs = 100.0)))
        assertEquals(Words.WRITING, say(ready.copy(running = true, writing = true, flashWaitMs = 30.0, wallMs = 100.0)))
        assertEquals(Words.ANSWERED, say(ready.copy(lastIsAnswer = true)))
        assertEquals(Words.STOPPED, say(ready.copy(lastIsAnswer = true, cancelled = true)))
        assertEquals(Words.READY, say(ready.copy(cancelled = true)))
        assertEquals(Words.FAILED, say(ready.copy(failed = true, lastIsAnswer = true)))
        assertTrue(ConversationStatus.of(ready.copy(running = true)).mood == Mood.BUSY)
    }

    @Test
    fun theWelcomeGreetsOnlyWhenAllIsReady() {
        assertEquals(Words.PRIVATE, ConversationStatus.of(ready, greet = true).text)
        assertEquals(Words.NO_MODEL, ConversationStatus.of(ready.copy(engineState = "empty"), greet = true).text)
        assertEquals(Words.LOADING, ConversationStatus.of(ready.copy(engineState = "loading"), greet = true).text)
    }

    @Test
    fun theLiveRateIsSmoothed() {
        assertEquals(10.0, TurnFigures.ema(null, 10.0), 1e-9)
        assertEquals(12.0, TurnFigures.ema(10.0, 20.0), 1e-9)
    }
}
