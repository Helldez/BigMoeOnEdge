package io.bigmoeonedge.app

import io.bigmoeonedge.app.data.AppPrefs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** The theme choice: the web UI's three words, and what each draws. */
class AppPrefsTest {
    @Test
    fun anUnknownOrMissingThemeFollowsTheSystem() {
        assertEquals(listOf("System", "Light", "Dark"), AppPrefs.THEMES.map { it.second })
        assertEquals("system", AppPrefs.themeOf(null))
        assertEquals("system", AppPrefs.themeOf("sepia"))
        assertEquals("dark", AppPrefs.themeOf("dark"))
    }

    @Test
    fun lightAndDarkOverrideTheSystem() {
        assertTrue(AppPrefs.darkFor("system", true))
        assertFalse(AppPrefs.darkFor("system", false))
        assertFalse(AppPrefs.darkFor("light", true))
        assertTrue(AppPrefs.darkFor("dark", false))
    }
}
