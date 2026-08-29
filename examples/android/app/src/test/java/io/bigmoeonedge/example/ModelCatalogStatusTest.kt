package io.bigmoeonedge.example

import io.bigmoeonedge.example.ModelCatalog.Entry
import io.bigmoeonedge.example.ModelCatalog.Shard
import io.bigmoeonedge.example.ModelCatalog.Status
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The on-device rule for sharded catalog entries. A set is on-device only when every shard is; the
 * first shard alone (the file the engine opens, and the entry's fileName) is not enough, because it
 * is the smallest and lands seconds into a multi-hour download.
 */
class ModelCatalogStatusTest {
    private val shards = listOf(
        Shard("m-00001-of-00003.gguf", "https://example.invalid/1", 1L),
        Shard("m-00002-of-00003.gguf", "https://example.invalid/2", 2L),
        Shard("m-00003-of-00003.gguf", "https://example.invalid/3", 3L),
    )

    /** fileName is the first shard, the shape every sharded entry has since gpt-oss. */
    private val split = Entry(
        title = "m", quant = "q", fileName = shards[0].fileName, approxBytes = 6L, url = null,
        blurb = "", shards = shards,
    )

    /** fileName is a legacy merged file that is not one of the shards (gpt-oss). */
    private val legacy = split.copy(fileName = "m.gguf")

    @Test
    fun firstShardAloneIsNotOnDevice() {
        assertEquals(Status.AVAILABLE, ModelCatalog.statusOf(split, setOf(shards[0].fileName), emptySet()))
    }

    @Test
    fun firstShardWithTheRestInFlightIsDownloading() {
        val status = ModelCatalog.statusOf(split, setOf(shards[0].fileName), setOf(shards[1].fileName))
        assertEquals(Status.DOWNLOADING, status)
    }

    @Test
    fun everyShardPresentIsOnDevice() {
        assertEquals(Status.ON_DEVICE, ModelCatalog.statusOf(split, shards.map { it.fileName }.toSet(), emptySet()))
    }

    @Test
    fun legacyMergedFileIsOnDevice() {
        assertEquals(Status.ON_DEVICE, ModelCatalog.statusOf(legacy, setOf("m.gguf"), emptySet()))
    }

    @Test
    fun legacyEntryWithOnlyTheFirstShardIsNotOnDevice() {
        assertEquals(Status.AVAILABLE, ModelCatalog.statusOf(legacy, setOf(shards[0].fileName), emptySet()))
    }
}
