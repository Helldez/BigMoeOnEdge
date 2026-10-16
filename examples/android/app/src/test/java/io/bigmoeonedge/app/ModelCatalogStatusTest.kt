package io.bigmoeonedge.app

import io.bigmoeonedge.app.data.CatalogEntry
import io.bigmoeonedge.app.data.CatalogFile
import io.bigmoeonedge.app.data.CatalogStatus
import io.bigmoeonedge.app.data.catalogStatus
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The on-device rule for a catalog entry. The server answers for its own directory and lists the
 * files of every other place it was started with; the app adds its transfers in flight. A set is
 * on the device only when every file is: the first shard alone lands seconds into a multi-hour
 * download.
 */
class ModelCatalogStatusTest {
    private val files = listOf(
        CatalogFile("m-00001-of-00003.gguf", 1L, "https://example.invalid/1"),
        CatalogFile("m-00002-of-00003.gguf", 2L, "https://example.invalid/2"),
        CatalogFile("m-00003-of-00003.gguf", 3L, "https://example.invalid/3"),
    )
    private val split = CatalogEntry("m", "m", "q", "", 6L, "available", files)

    @Test
    fun firstShardAloneIsNotOnDevice() {
        assertEquals(CatalogStatus.AVAILABLE, catalogStatus(split, setOf(files[0].name), emptySet()))
    }

    @Test
    fun firstShardWithTheRestInFlightIsDownloading() {
        assertEquals(CatalogStatus.DOWNLOADING, catalogStatus(split, setOf(files[0].name), setOf(files[1].name)))
    }

    @Test
    fun everyShardPresentIsOnDevice() {
        assertEquals(CatalogStatus.ON_DEVICE, catalogStatus(split, files.map { it.name }.toSet(), emptySet()))
    }

    @Test
    fun theServerSeeingItOnDiskIsEnough() {
        assertEquals(CatalogStatus.ON_DEVICE, catalogStatus(split.copy(serverStatus = "on_disk"), emptySet(), emptySet()))
    }

    @Test
    fun aTransferInFlightOutranksTheServersView() {
        assertEquals(CatalogStatus.DOWNLOADING, catalogStatus(split.copy(serverStatus = "on_disk"), emptySet(), setOf(files[2].name)))
    }
}
