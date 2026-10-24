package io.bigmoeonedge.app

import io.bigmoeonedge.app.engine.ModelInfo
import io.bigmoeonedge.app.engine.ParamSpec
import io.bigmoeonedge.app.engine.PlanResult
import io.bigmoeonedge.app.engine.appliedChanges
import io.bigmoeonedge.app.engine.contextWarning
import io.bigmoeonedge.app.engine.diagnosticText
import io.bigmoeonedge.app.engine.planFigures
import io.bigmoeonedge.app.engine.text
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The Plan screen's four figures and what Apply changed, and the context warning, from the server's words. */
class PlanAndContextTest {
    private val table = ParamSpec.parse(
        JSONArray(
            """[
              {"key":"cache","label":"Expert cache","type":"int","group":"c","group_label":"C","level":"basic","unit":"MiB","default":"auto"},
              {"key":"mode","label":"Dense weights","type":"choice","group":"d","group_label":"D","level":"basic",
               "choices":[{"value":"anon","label":"held in memory"},{"value":"mmap","label":"read from the file"}],"default":"anon"},
              {"key":"lanes","label":"Read lanes","type":"int","group":"s","group_label":"S","level":"advanced","special":[{"value":0,"label":"off"}],"default":4},
              {"key":"ctx","label":"Context","type":"int","group":"g","group_label":"G","level":"basic","default":4096}
            ]""",
        ),
    )

    @Test
    fun theFourFiguresComeFromThePredictionTheMeasurementAndTheHostPool() {
        val p = PlanResult.of(
            JSONObject(
                """{"available":true,"predicted":{"s_per_token":0.5,"cache_hit_pct":40.0,"s_per_token_worst":2.0},
                    "measured":{"s_per_token":0.4,"cache_hit_pct":45.0,"tokens":20},
                    "memory":{"pools":[{"name":"gpu","cap_bytes":10,"held_bytes":1,"rows":[]},
                                       {"name":"host","label":"This process","cap_bytes":1000,"held_bytes":600,"rows":[]}]}}""",
            ),
        )
        val f = planFigures(p)
        assertEquals(2.0, f.predictedTokS!!, 1e-9)
        assertEquals(2.5, f.measuredTokS!!, 1e-9)
        assertEquals(25.0, f.deltaPct!!, 1e-9)
        assertEquals(40.0, f.creditedHitPct!!, 1e-9)
        assertEquals(45.0, f.measuredHitPct!!, 1e-9)
        assertEquals(600L, f.heldBytes)
        assertEquals(1000L, f.capBytes)
        assertEquals(2000.0, f.worstMs!!, 1e-9)
        assertEquals(500.0, f.expectedMs!!, 1e-9)

        val bare = planFigures(PlanResult.of(JSONObject("""{"available":true,"measured":{"s_per_token":0.4,"cache_hit_pct":-1}}""")))
        assertNull(bare.predictedTokS)
        assertNull(bare.deltaPct)
        assertNull(bare.measuredHitPct)
        assertNull(bare.capBytes)
        assertNull(bare.worstMs)
    }

    @Test
    fun applyListsWhatChangedInTableOrderWordedFromTheTable() {
        val before = mapOf<String, Any?>("cache" to "auto", "mode" to "anon", "lanes" to 4L, "ctx" to 4096L)
        val after = mapOf<String, Any?>("cache" to 2048L, "mode" to "mmap", "lanes" to 0L, "ctx" to 4096.0, "later" to 1L)
        val changes = appliedChanges(before, after, table)
        assertEquals(listOf("cache", "mode", "lanes", "later"), changes.map { it.key })
        assertEquals("2048 MiB", changes[0].to)
        assertEquals("auto", changes[0].from)
        assertEquals("held in memory", changes[1].from)
        assertEquals("read from the file", changes[1].to)
        assertEquals("off", changes[2].to)
        // A key the table does not have is still listed, under its own name.
        assertEquals("later", changes[3].label)
    }

    @Test
    fun theDiagnosticIsTheCommandAndTheWholeAccount() {
        assertEquals("bmoe-cli --a 1\n\nline\n", diagnosticText(listOf("--a", "1"), " line \n"))
        assertTrue(diagnosticText(emptyList(), "").startsWith("(no arguments"))
    }

    @Test
    fun theContextWarningIsTheServersAdviceLabelledFromTheTable() {
        val m = ModelInfo.of(JSONObject("""{"name":"m","path":"/m/m.gguf","n_ctx":4096,"ctx_advice":{"recommended":16384,"short":true,"param":"ctx","warn":true}}"""))
        val w = contextWarning(m, table)!!
        assertEquals(4096, w.nCtx)
        assertEquals(16384, w.recommended)
        assertTrue(w.short)
        assertTrue(w.warn)
        assertEquals("/m/m.gguf\n4096", w.dismissKey)
        // Short on a server only this phone reaches (or one that does not say) is no warning.
        val local = ModelInfo.of(JSONObject("""{"name":"m","n_ctx":4096,"ctx_advice":{"recommended":16384,"short":true,"param":"ctx"}}"""))
        assertFalse(contextWarning(local, table)!!.warn)
        // A refusal is another warning: it comes back after a dismissal and says what happened.
        val refused = ModelInfo.of(JSONObject("""{"name":"m","path":"/m/m.gguf","n_ctx":4096,"ctx_advice":{"recommended":16384,"short":true,"param":"ctx","warn":true,"refused":true}}"""))
        val r = contextWarning(refused, table)!!
        assertTrue(r.refused)
        assertTrue(r.dismissKey != w.dismissKey)
        assertTrue(r.text().startsWith("A request was refused"))
        assertTrue(w.text().contains("16384") && w.text().contains("4096"))
        assertEquals("Context", w.label)
        assertEquals("ctx", w.key)
        // A parameter the table does not have is named by its key; no advice is no warning.
        assertEquals("ctx", contextWarning(m, emptyList())!!.label)
        assertNull(contextWarning(ModelInfo.of(JSONObject("""{"name":"m","n_ctx":4096}""")), table))
        assertNull(contextWarning(null, table))
    }
}
