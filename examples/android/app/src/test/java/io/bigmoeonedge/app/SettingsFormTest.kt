package io.bigmoeonedge.app

import io.bigmoeonedge.app.engine.ConfigState
import io.bigmoeonedge.app.engine.ParamSpec
import io.bigmoeonedge.app.engine.PlanDecision
import io.bigmoeonedge.app.engine.ValueSource
import io.bigmoeonedge.app.engine.differsFromPlan
import io.bigmoeonedge.app.engine.levelCounts
import io.bigmoeonedge.app.engine.loadTargetChosen
import io.bigmoeonedge.app.engine.pinnedKeys
import io.bigmoeonedge.app.engine.resettableKeys
import io.bigmoeonedge.app.engine.sameValue
import io.bigmoeonedge.app.engine.settingsCards
import io.bigmoeonedge.app.engine.valueSource
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** The settings form as one card per group, worked out from the parameter table alone. */
class SettingsFormTest {
    private val table = ParamSpec.parse(
        JSONArray(
            """[
              {"key":"model","label":"Model","type":"path","group":"model","group_label":"Model","group_order":0,
               "group_read_only":true,"level":"basic","help":"The gguf."},
              {"key":"io","label":"Read lanes","type":"int","group":"stream","group_label":"Streaming","group_order":2,
               "group_help":"How experts are read.","level":"advanced","help":"Parallel reads.","flag":"--io","default":4},
              {"key":"cache","label":"Expert cache","type":"int","group":"cache","group_label":"Cache","group_order":1,
               "level":"basic","help":"How big the cache is.","default":"auto"},
              {"key":"dbg","label":"Trace","type":"bool","group":"cache","group_label":"Cache","group_order":1,
               "level":"debug","help":"Logs every read.","flag":"--trace","default":false},
              {"key":"exp","label":"Drop cold","type":"float","group":"cache","group_label":"Cache","group_order":1,
               "level":"experimental","help":"Skips experts.","default":0}
            ]""",
        ),
    )

    @Test
    fun cardsFollowTheGroupOrderWithBasicRowsOpenAndTheRestByLevel() {
        val cards = settingsCards(table, upTo = "debug")
        assertEquals(listOf("model", "cache", "stream"), cards.map { it.group })
        assertTrue(cards[0].readOnly)
        val cache = cards[1]
        assertEquals(listOf("cache"), cache.basic.map { it.key })
        assertEquals(listOf("experimental", "debug"), cache.more.map { it.level })
        assertEquals(2, cache.moreCount)
        assertEquals("How experts are read.", cards[2].help)
        assertTrue(cards[2].basic.isEmpty())
    }

    @Test
    fun theFilterHidesDeeperRowsAndCountsThemButNeverAPinnedOne() {
        val basic = settingsCards(table, upTo = "basic")
        // A group left with no row is no card; the cache card says how many rows the filter left out.
        assertEquals(listOf("model", "cache"), basic.map { it.group })
        assertEquals(2, basic[1].hidden)
        val pinned = settingsCards(table, upTo = "basic", pinned = setOf("dbg"))
        assertEquals(listOf("dbg"), pinned[1].more.flatMap { lv -> lv.params.map { it.key } })
        assertEquals(1, pinned[1].hidden)
        assertEquals(mapOf("basic" to 2, "advanced" to 1, "debug" to 1, "experimental" to 1), levelCounts(table))
    }

    @Test
    fun aSearchLooksAtEveryLevelByLabelHelpAndFlag() {
        assertEquals(listOf("dbg"), settingsCards(table, upTo = "basic", query = "--TRACE").flatMap { c -> c.more.flatMap { lv -> lv.params.map { it.key } } })
        assertEquals(listOf("stream"), settingsCards(table, upTo = "basic", query = "parallel").map { it.group })
        assertTrue(settingsCards(table, upTo = "debug", query = "nothing like this").isEmpty())
    }

    @Test
    fun resetAllLeavesTheLaunchFlagsAndTheReadOnlyGroup() {
        assertEquals(listOf("cache"), resettableKeys(listOf("model", "io", "cache"), setOf("io"), table))
    }

    @Test
    fun aValueComesFromThePersonThePlanOrTheDefault() {
        val decided = PlanDecision("cache", "4096", "derived", "r", "Expert cache", "set")
        val kept = decided.copy(group = "kept")
        assertEquals(ValueSource.YOU, valueSource(true, decided))
        assertEquals(ValueSource.PLAN, valueSource(false, decided))
        assertEquals(ValueSource.DEFAULT, valueSource(false, kept))
        assertEquals(ValueSource.DEFAULT, valueSource(false, null))
        // "Use plan value" only where the person's value is not the plan's.
        assertTrue(differsFromPlan(2048L, decided))
        assertFalse(differsFromPlan(4096L, decided))
        assertFalse(differsFromPlan(4096.0, decided))
        assertFalse(differsFromPlan(2048L, kept))
        assertTrue(sameValue(8, "8"))
        assertTrue(sameValue(8L, 8.0))
        assertFalse(sameValue("auto", 4096))
    }

    @Test
    fun rowsSetBySomeoneOrThePlanOrOffTheirDefaultArePinned() {
        val cfg = ConfigState.of(
            JSONObject(
                """{"values":{"model":"/m.gguf","io":4,"cache":"auto","dbg":true,"exp":0},"user_keys":["model"],
                    "launch_keys":["model"],"reload_keys":["dbg"],"plan":[{"knob":"exp","value":"0","group":"set"}]}""",
            ),
        )
        assertEquals(setOf("model"), cfg.launchKeys)
        assertEquals(listOf("dbg"), cfg.reloadKeys)
        assertEquals(setOf("model", "dbg", "exp"), pinnedKeys(table, cfg))
    }

    @Test
    fun whatALoadOpensIsTheReadOnlyGroupsValue() {
        val chosen = ConfigState.of(JSONObject("""{"values":{"model":"/m.gguf","io":4}}"""))
        val none = ConfigState.of(JSONObject("""{"values":{"model":"","io":4}}"""))
        assertTrue(loadTargetChosen(table, chosen))
        assertFalse(loadTargetChosen(table, none))
        assertFalse(loadTargetChosen(null, chosen))
        assertFalse(loadTargetChosen(table, null))
    }
}
