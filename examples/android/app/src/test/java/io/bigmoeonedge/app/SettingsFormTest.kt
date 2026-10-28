package io.bigmoeonedge.app

import io.bigmoeonedge.app.engine.ConfigState
import io.bigmoeonedge.app.engine.ParamSpec
import io.bigmoeonedge.app.engine.PlanDecision
import io.bigmoeonedge.app.engine.ValueSource
import io.bigmoeonedge.app.engine.differsFromPlan
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
               "group_help":"How experts are read.","level":"advanced","form":true,"help":"Parallel reads.","flag":"--io","default":4},
              {"key":"cache","label":"Expert cache","type":"int","group":"cache","group_label":"Cache","group_order":1,
               "level":"basic","help":"How big the cache is.","default":"auto"},
              {"key":"warm","label":"Warm start","type":"bool","group":"cache","group_label":"Cache","group_order":1,
               "level":"basic","form":false,"help":"Reads the cache in ahead.","default":false},
              {"key":"dbg","label":"Trace","type":"bool","group":"cache","group_label":"Cache","group_order":1,
               "level":"debug","help":"Logs every read.","flag":"--trace","default":false},
              {"key":"exp","label":"Drop cold","type":"float","group":"cache","group_label":"Cache","group_order":1,
               "level":"experimental","help":"Skips experts.","default":0}
            ]""",
        ),
    )

    @Test
    fun cardsFollowTheGroupOrderAndListTheFormRowsInTableOrder() {
        val cards = settingsCards(table)
        assertEquals(listOf("model", "cache", "stream"), cards.map { it.group })
        assertTrue(cards[0].readOnly)
        // "form" decides, whatever the level; without it, a basic row is a form row.
        assertEquals(listOf("cache"), cards[1].params.map { it.key })
        assertEquals(listOf("io"), cards[2].params.map { it.key })
        assertEquals("How experts are read.", cards[2].help)
    }

    @Test
    fun aRowThePersonSetIsShownEvenOffTheForm() {
        val pinned = settingsCards(table, pinned = setOf("dbg", "warm"))
        assertEquals(listOf("cache", "warm", "dbg"), pinned[1].params.map { it.key })
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
    fun onlyTheRowsThePersonSetArePinned() {
        val cfg = ConfigState.of(
            JSONObject(
                """{"values":{"model":"/m.gguf","io":4,"cache":"auto","dbg":true,"exp":0},"user_keys":["model"],
                    "launch_keys":["model"],"reload_keys":["dbg"],"plan":[{"knob":"exp","value":"0","group":"set"}]}""",
            ),
        )
        assertEquals(setOf("model"), cfg.launchKeys)
        assertEquals(listOf("dbg"), cfg.reloadKeys)
        assertEquals(setOf("model"), pinnedKeys(table, cfg))
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
