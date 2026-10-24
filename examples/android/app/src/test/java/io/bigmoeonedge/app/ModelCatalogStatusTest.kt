package io.bigmoeonedge.app

import io.bigmoeonedge.app.data.CatalogEntry
import io.bigmoeonedge.app.data.CatalogFile
import io.bigmoeonedge.app.data.RowState
import io.bigmoeonedge.app.data.isPlainFileName
import io.bigmoeonedge.app.data.isSha256Hex
import io.bigmoeonedge.app.data.libraryRows
import io.bigmoeonedge.app.engine.LocalModel
import io.bigmoeonedge.app.engine.ModelsList
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The state of a catalog entry in the one list. The server answers for its own directory (the
 * entry's `status`, and the files it lists there); the app adds its transfers in flight. A set is
 * on the device only when every file is: the first shard alone lands seconds into a multi-hour
 * download. A copy of the file in another directory is a model of its own, not the entry's.
 */
class ModelCatalogStatusTest {
    private val files = listOf(
        CatalogFile("m-00001-of-00003.gguf", 1L, "https://example.invalid/1"),
        CatalogFile("m-00002-of-00003.gguf", 2L, "https://example.invalid/2"),
        CatalogFile("m-00003-of-00003.gguf", 3L, "https://example.invalid/3"),
    )
    private val split = CatalogEntry("m", "m", "q", "", 6L, "available", files)

    private fun local(dir: String, complete: Boolean) =
        LocalModel(files[0].name, "$dir/${files[0].name}", dir, "", 1L, 3, listOf("$dir/${files[0].name}"), "a", true, complete, "model")

    private fun state(entry: CatalogEntry, local: List<LocalModel>, downloading: Set<String> = emptySet()): RowState {
        val list = ModelsList("/models", listOf("/pushed"), local, listOf(entry))
        return libraryRows(list, local, downloading, null).first { it.entry?.id == entry.id }.state
    }

    @Test
    fun firstShardAloneIsNotOnDevice() {
        assertEquals(RowState.INCOMPLETE, state(split, listOf(local("/models", complete = false))))
    }

    @Test
    fun firstShardWithTheRestInFlightIsDownloading() {
        assertEquals(RowState.DOWNLOADING, state(split, listOf(local("/models", complete = false)), setOf(files[1].name)))
    }

    @Test
    fun aCopyInAnotherDirectoryDoesNotClaimTheEntry() {
        val rows = libraryRows(ModelsList("/models", listOf("/pushed"), listOf(local("/pushed", true)), listOf(split)), listOf(local("/pushed", true)), emptySet(), null)
        assertEquals(listOf(RowState.ON_DEVICE, RowState.AVAILABLE), rows.map { it.state })
        assertNull(rows[0].entry)
        assertTrue(rows[1].suggested)
    }

    @Test
    fun theServerSeeingItOnDiskIsEnough() {
        assertEquals(RowState.ON_DEVICE, state(split.copy(serverStatus = "on_disk"), emptyList()))
    }

    @Test
    fun aTransferInFlightOutranksTheServersView() {
        assertEquals(RowState.DOWNLOADING, state(split.copy(serverStatus = "on_disk"), emptyList(), setOf(files[2].name)))
    }

    private fun entryJson(name: String, sha256: String? = null): JSONObject {
        val f = JSONObject().put("name", name).put("bytes", 3).put("url", "https://example.invalid/m")
        if (sha256 != null) f.put("sha256", sha256)
        return JSONObject().put("id", "m").put("files", org.json.JSONArray().put(f))
    }

    @Test
    fun aFileNameThatLeavesTheModelsDirDropsTheEntry() {
        for (bad in listOf("../m.gguf", "a/m.gguf", "a\\m.gguf", "..", ".", "", "C:m.gguf")) {
            assertFalse(bad, isPlainFileName(bad))
            assertNull(bad, CatalogEntry.of(entryJson(bad)))
        }
        assertTrue(isPlainFileName("m-00001-of-00003.gguf"))
    }

    @Test
    fun theDigestIsReadLowercasedAndAMalformedOneDropsTheEntry() {
        val hex = "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"
        assertEquals(hex.lowercase(), CatalogEntry.of(entryJson("m.gguf", hex))!!.files[0].sha256)
        assertEquals("", CatalogEntry.of(entryJson("m.gguf"))!!.files[0].sha256)
        assertNull(CatalogEntry.of(entryJson("m.gguf", "abc")))
        assertTrue(isSha256Hex(hex.lowercase()))
    }
}
