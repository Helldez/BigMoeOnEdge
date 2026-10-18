package io.bigmoeonedge.app

import io.bigmoeonedge.app.data.HfFacts
import io.bigmoeonedge.app.data.HfModels
import io.bigmoeonedge.app.data.HfSearch
import io.bigmoeonedge.app.data.hfSearchBody
import io.bigmoeonedge.app.data.hfRequestBody
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** The Hugging Face answers as the server sends them (docs/server-api.md, "Models"), read into what the screen shows. */
class HfRepoTest {
    @Test
    fun aListingReadsEveryModelWithItsShards() {
        val r = HfModels.of(
            JSONObject(
                """{"repo":"ggml-org/x-GGUF","revision":"main","file":"",
                    "models":[{"name":"Q2_K/x-Q2_K-00001-of-00003.gguf","quant":"Q2_K","bytes":300,"count":3,"complete":false,"files":[]},
                              {"name":"x-Q4_K_M.gguf","quant":"Q4_K_M","bytes":20419565568,"count":1,"complete":true,"files":[]}]}""",
            ),
        )
        assertEquals("ggml-org/x-GGUF", r.repo)
        assertEquals(2, r.models.size)
        assertEquals(3, r.models[0].count)
        assertFalse(r.models[0].complete)
        assertEquals(20419565568L, r.models[1].bytes)
        assertEquals("Q4_K_M", r.models[1].quant)
    }

    @Test
    fun theFactsCarryTheModelAndTheVerdict() {
        val f = HfFacts.of(
            JSONObject(
                """{"name":"x-Q4_K_M.gguf","quant":"Q4_K_M","bytes":100,"files":1,"complete":true,
                    "model":{"readable":true,"arch":"qwen35moe","is_moe":true,"n_expert":256,"n_expert_used":8,"n_layer":40,"n_moe_layer":40,
                             "dense_bytes":1872000000,"expert_bytes":19500000000,"token_cycle_bytes":609000000,"streamable":true,"refusal":""},
                    "machine":{"residency_budget":4000000000,"memory_total":12000000000,"disk_free":50000000000,"present_bytes":0},
                    "verdict":{"downloadable":true,"mode":"streams","disk":"needs 100 MiB","memory":"the experts stream"}}""",
            ),
        )
        assertTrue(f.readable && f.isMoe && f.streamable && f.downloadable)
        assertEquals("qwen35moe", f.arch)
        assertEquals(256, f.nExpert)
        assertEquals(8, f.nExpertUsed)
        assertEquals("streams", f.mode)
        assertEquals("needs 100 MiB", f.disk)
    }

    @Test
    fun anUnreadableHeaderIsAFactNotACrash() {
        val f = HfFacts.of(JSONObject("""{"name":"x.gguf","bytes":1,"model":{"readable":false},"verdict":{"downloadable":true,"mode":"not readable","disk":"","memory":""}}"""))
        assertFalse(f.readable)
        assertEquals("not readable", f.mode)
    }

    @Test
    fun aSearchReadsItsRepositories() {
        val s = HfSearch.of(JSONObject("""{"query":"gemma","repos":[{"id":"unsloth/gemma-4-26B-A4B-it-GGUF","downloads":886490,"likes":1224,"modified":"2026-10-01","gated":false},{"id":"google/x","gated":true}]}"""))
        assertEquals("gemma", s.query)
        assertEquals(2, s.repos.size)
        assertEquals(886490L, s.repos[0].downloads)
        assertFalse(s.repos[0].gated)
        assertTrue(s.repos[1].gated)
        assertEquals("gemma 4", hfSearchBody(" gemma 4 ").getString("query"))
        assertFalse(hfSearchBody("x", " ").has("token"))
    }

    @Test
    fun theRequestCarriesOnlyWhatWasGiven() {
        val plain = hfRequestBody(" ggml-org/x-GGUF ")
        assertEquals("ggml-org/x-GGUF", plain.getString("repo"))
        assertFalse(plain.has("name"))
        assertFalse(plain.has("token"))
        val full = hfRequestBody("a/b", "x.gguf", " tok ")
        assertEquals("x.gguf", full.getString("name"))
        assertEquals("tok", full.getString("token"))
        assertFalse(hfRequestBody("a/b", "", "  ").has("token"))
    }
}
