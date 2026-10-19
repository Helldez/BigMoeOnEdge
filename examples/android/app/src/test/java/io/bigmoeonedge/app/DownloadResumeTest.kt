package io.bigmoeonedge.app

import io.bigmoeonedge.app.data.CatalogEntry
import io.bigmoeonedge.app.data.bytesStillNeeded
import io.bigmoeonedge.app.data.freeSpaceRefusal
import io.bigmoeonedge.app.data.resumeLabel
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * A stopped download, on the phone as on the desktop: a resume needs only the bytes its `.part`
 * lacks, and a row with a `.part` reads "Resume (X of Y GB)" with the same words the web UI uses.
 */
class DownloadResumeTest {
    @Test
    fun aResumeNeedsOnlyWhatThePartLacks() {
        assertEquals(14_000_000_000L, bytesStillNeeded(47_000_000_000L, 33_000_000_000L))
        assertEquals(0L, bytesStillNeeded(10L, 12L))
    }

    @Test
    fun aResumeThatFitsIsNotRefusedForTheWholeModel() {
        // 47 GB model, 33 GB already in the .part, 20 GB free: the remaining 14 GB fit.
        assertNull(freeSpaceRefusal(bytesStillNeeded(47_000_000_000L, 33_000_000_000L), 20_000_000_000L))
        assertEquals("needs 14.0 GB more, only 3.0 GB free", freeSpaceRefusal(14_000_000_000L, 3_000_000_000L))
    }

    @Test
    fun theLabelMatchesTheWebUi() {
        assertEquals("Resume (14.2 of 47.0 GB)", resumeLabel(14_230_000_000L, 47_000_000_000L))
    }

    @Test
    fun theServerSaysWhatIsAlreadyOnDisk() {
        val e = CatalogEntry.of(JSONObject("""
            {"id":"m","title":"M","bytes":47,"status":"available","received":33,"partial":true,
             "files":[{"name":"m.gguf","bytes":47,"url":"https://example.invalid/m.gguf"}]}
        """.trimIndent()))!!
        assertTrue(e.partial)
        assertEquals(33L, e.received)
    }

    @Test
    fun anOlderServerReadsAsNothingToResume() {
        val e = CatalogEntry.of(JSONObject("""
            {"id":"m","title":"M","bytes":47,"status":"available",
             "files":[{"name":"m.gguf","bytes":47,"url":"https://example.invalid/m.gguf"}]}
        """.trimIndent()))!!
        assertFalse(e.partial)
        assertEquals(0L, e.received)
    }
}
