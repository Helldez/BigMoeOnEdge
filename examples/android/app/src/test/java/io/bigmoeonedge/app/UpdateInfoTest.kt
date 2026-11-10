package io.bigmoeonedge.app

import io.bigmoeonedge.app.engine.UpdateInfo
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** The update check reads what the server answered: the phone has no installer in the feed, yet is told it is behind. */
class UpdateInfoTest {
    @Test
    fun aNewerReleaseWithNoInstallerForThePhoneIsStillAvailable() {
        val u = UpdateInfo.of(
            JSONObject(
                """{"current":"0.40.1","latest":"0.40.2","available":true,"installer":null,""" +
                    """"notes":"https://github.com/o/r/releases/tag/v0.40.2","target":"android-arm64"}""",
            ),
        )
        assertTrue(u.available)
        assertEquals("0.40.2", u.latest)
        assertEquals("https://github.com/o/r/releases/tag/v0.40.2", u.releasePage)
    }

    @Test
    fun onlyAnHttpsPageIsOpened() {
        assertEquals(null, UpdateInfo.of(JSONObject("""{"available":true,"notes":"intent://x"}""")).releasePage)
    }

    @Test
    fun theSameVersionIsNotAvailable() {
        val u = UpdateInfo.of(JSONObject("""{"current":"0.40.2","latest":"0.40.2","available":false}"""))
        assertFalse(u.available)
        assertEquals("", u.notes)
    }
}
