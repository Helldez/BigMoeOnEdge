package io.bigmoeonedge.app

import io.bigmoeonedge.app.data.CatalogEntry
import io.bigmoeonedge.app.data.CatalogFile
import io.bigmoeonedge.app.data.Eta
import io.bigmoeonedge.app.data.Eta.Sample
import io.bigmoeonedge.app.data.RowState
import io.bigmoeonedge.app.data.Setup
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The first run: the time a download has left (the web's ui/src/lib/eta.test.ts), the models offered, the step. */
class SetupTest {
    @Test
    fun measuresTheRateOverTheWindowAndTheTimeLeft() {
        var s = Eta.addSample(emptyList(), Sample(0, 0))
        s = Eta.addSample(s, Sample(5000, 50_000_000))
        assertEquals(10_000_000.0, Eta.rate(s)!!, 1e-9)
        assertEquals(10.0, Eta.secondsLeft(s, 150_000_000)!!, 1e-9)
    }

    @Test
    fun knowsNothingFromOneSampleOrSamplesLessThanASecondApart() {
        assertNull(Eta.rate(listOf(Sample(0, 0))))
        assertNull(Eta.rate(listOf(Sample(0, 0), Sample(500, 10))))
        assertNull(Eta.secondsLeft(listOf(Sample(0, 0), Sample(2000, 0)), 100)) // no progress: no estimate
    }

    @Test
    fun dropsSamplesOlderThanTheWindowAndAnyFromBeforeARestart() {
        var s = Eta.addSample(emptyList(), Sample(0, 0))
        s = Eta.addSample(s, Sample(30_000, 100))
        assertEquals(1, s.size)
        s = Eta.addSample(listOf(Sample(0, 500)), Sample(1000, 10))
        assertEquals(1, s.size)
    }

    @Test
    fun roundsLikeAPerson() {
        assertEquals("less than a minute", Eta.humanDuration(20.0))
        assertEquals("about 12 min", Eta.humanDuration(12 * 60 + 10.0))
        assertEquals("about 1 h 20 min", Eta.humanDuration(80 * 60.0))
        assertEquals("about 2 h", Eta.humanDuration(120 * 60.0))
    }

    private fun entry(id: String, gb: Long, added: Boolean = false) =
        CatalogEntry(id, id, "Q4_K_M", "", gb * 1_000_000_000L, "available", listOf(CatalogFile("$id.gguf", gb * 1_000_000_000L, "")), added = added)

    private val catalog = listOf(entry("small", 5), entry("mid", 9), entry("big", 18), entry("huge", 30), entry("mine", 10, added = true))

    @Test
    fun offersEveryShippedModelAndSuggestsTheLargestThatFits() {
        val ram = 12_000_000_000L
        assertEquals(listOf("small", "mid", "big", "huge"), Setup.choices(catalog.reversed()).map { it.id })
        assertEquals("mid", Setup.recommended(catalog, ram)?.id)
        // Nothing fits: the smallest is suggested, among them all.
        assertEquals("small", Setup.recommended(catalog, 1)?.id)
        // One model: no list, the suggestion stands alone.
        assertEquals(emptyList<CatalogEntry>(), Setup.choices(listOf(entry("small", 5), entry("mine", 10, added = true))))
        assertNull(Setup.recommended(listOf(entry("mine", 10, added = true)), 12_000_000_000L))
    }

    @Test
    fun theStepFollowsTheModelsState() {
        assertEquals(1, Setup.step(null, loading = false))
        assertEquals(1, Setup.step(RowState.AVAILABLE, loading = false))
        assertEquals(1, Setup.step(RowState.PARTIAL, loading = false))
        assertEquals(2, Setup.step(RowState.DOWNLOADING, loading = false))
        assertEquals(3, Setup.step(RowState.ON_DEVICE, loading = false))
        assertEquals(3, Setup.step(RowState.LOADED, loading = false))
        assertEquals(3, Setup.step(RowState.AVAILABLE, loading = true))
        // A paused download stays on the download step, unless the person asked to pick again.
        assertTrue(Setup.paused(partial = true, RowState.PARTIAL))
        assertFalse(Setup.paused(partial = true, RowState.DOWNLOADING))
        assertFalse(Setup.paused(partial = false, RowState.AVAILABLE))
        assertEquals(2, Setup.step(RowState.PARTIAL, loading = false, paused = true))
        assertEquals(1, Setup.step(RowState.PARTIAL, loading = false, paused = true, repick = true))
        assertEquals(2, Setup.step(RowState.DOWNLOADING, loading = false, repick = true))
        assertEquals(3, Setup.step(RowState.ON_DEVICE, loading = false, paused = true))
        assertTrue(Setup.firstRun(0, downloading = false))
        assertFalse(Setup.firstRun(0, downloading = true))
        assertFalse(Setup.firstRun(1, downloading = false))
    }
}
