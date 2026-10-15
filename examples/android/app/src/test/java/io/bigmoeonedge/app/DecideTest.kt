package io.bigmoeonedge.app

import io.bigmoeonedge.app.engine.ChatMessage
import io.bigmoeonedge.app.engine.ChatProtocol
import io.bigmoeonedge.app.engine.ChoiceScore
import io.bigmoeonedge.app.engine.Decide
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.ln

/** Choose from options: the request's wording and the answer's reading, same as the web UI's. */
class DecideTest {
    @Test
    fun theRequestLettersTheOptions() {
        val options = Decide.parseOptions(" Osaka \n\nTokyo\nKyoto\n")
        assertEquals(listOf("Osaka", "Tokyo", "Kyoto"), options)
        val body = Decide.requestBody("Capital of Japan?", options)
        assertEquals("Capital of Japan?", body.getString("prefix"))
        assertEquals("\n\nOptions:\nA) Osaka\nB) Tokyo\nC) Kyoto\nAnswer with the letter of one option only.", body.getString("suffix"))
        assertEquals(listOf("A", "B", "C"), (0 until 3).map { body.getJSONArray("choices").getString(it) })
        assertEquals("Capital of Japan?\nA) Osaka\nB) Tokyo\nC) Kyoto", Decide.userText("Capital of Japan?", options))
    }

    @Test
    fun theAnswerReadsAsProbabilities() {
        val o = JSONObject(
            """{"cancelled":false,"best":1,"choice_logp":[${ln(0.1)},${ln(0.85)},null],
               "n_tokens":50,"n_reused":20,"n_prefilled":30,"prefill_s":1.26,"prefix_state_mib":48.2}""",
        )
        val r = Decide.parse(o, listOf("Osaka", "Tokyo", "Kyoto"))
        assertEquals(1, r.best)
        assertEquals(0.85, r.scores[1].prob, 1e-9)
        assertEquals(0.0, r.scores[2].prob, 0.0)
        assertEquals("prefill 1.3s (30 tok, 20 reused) · context kept 48 MiB", r.metrics)
        assertTrue(!r.cancelled)
    }

    @Test
    fun aDecisionStaysOutOfTheChatHistory() {
        val body = ChatProtocol.requestBody(
            listOf(
                ChatMessage("user", "hi"),
                ChatMessage("assistant", "hello"),
                ChatMessage("user", "Q\nA) x\nB) y"),
                ChatMessage("assistant", "", choices = listOf(ChoiceScore("A", "x", 0.9), ChoiceScore("B", "y", 0.1)), best = 0),
                ChatMessage("user", "and?"),
            ),
        )
        val msgs = body.getJSONArray("messages")
        assertEquals(listOf("hi", "hello", "and?"), (0 until msgs.length()).map { msgs.getJSONObject(it).getString("content") })
    }
}
