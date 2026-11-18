package io.bigmoeonedge.app

import io.bigmoeonedge.app.data.CheckState
import io.bigmoeonedge.app.data.ModelCheck
import io.bigmoeonedge.app.data.Tone
import io.bigmoeonedge.app.data.checkTone
import io.bigmoeonedge.app.data.mayDownload
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** The first run's check of a model: the server's sentence and tone, read and never re-decided. */
class ModelCheckTest {
    private fun check(verdict: String) = ModelCheck.of(JSONObject("""{"id":"m","bytes":1,"model":{"readable":true},"verdict":$verdict}"""))

    @Test
    fun readsTheServersSentenceAndTone() {
        val c = check("""{"downloadable":true,"mode":"streams","disk":"d","memory":"m","fit":"fits","headline":"Fits here","summary":"Runs here.","tone":"ok"}""")
        assertEquals("Runs here.", c.sentence)
        assertEquals(Tone.OK, checkTone(CheckState.Done(c)))
        // An older server has no plain sentence: its headline stands in, and no tone is claimed.
        val old = check("""{"downloadable":true,"mode":"streams","disk":"d","memory":"m","headline":"Fits here"}""")
        assertEquals("Fits here", old.sentence)
        assertEquals(Tone.NONE, checkTone(CheckState.Done(old)))
        assertEquals(Tone.NONE, checkTone(CheckState.Done(c.copy(tone = "a tone this build does not know"))))
        assertEquals(Tone.NONE, checkTone(CheckState.Pending))
        assertEquals(Tone.NONE, checkTone(null))
    }

    @Test
    fun holdsTheDownloadWhileCheckingAndWhereRefused() {
        val ok = check("""{"downloadable":true,"mode":"streams","disk":"","memory":""}""")
        assertFalse(mayDownload(null))
        assertFalse(mayDownload(CheckState.Pending))
        assertTrue(mayDownload(CheckState.Done(ok)))
        assertFalse(mayDownload(CheckState.Done(ok.copy(downloadable = false))))
        // A check that could not be made refuses nothing.
        assertTrue(mayDownload(CheckState.Failed("offline")))
    }
}
