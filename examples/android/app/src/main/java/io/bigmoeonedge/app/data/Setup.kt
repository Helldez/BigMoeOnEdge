package io.bigmoeonedge.app.data

/**
 * The first run's choices and where the person is on its path (docs/screens.md, "First run"; the
 * web UI's ui/src/routes/Setup.svelte): the catalog entry suggested for this phone's memory, the
 * two offered side by side, and the step, which follows the model's state and never a button.
 */
object Setup {
    /** The catalog entries a first run offers: the shipped ones (not added at run time) with a size. */
    private fun offered(catalog: List<CatalogEntry>) = catalog.filter { !it.added && it.bytes > 0 }

    /** The largest entry that fits in [ramBytes], or else the smallest; null with nothing offered. */
    fun recommended(catalog: List<CatalogEntry>, ramBytes: Long): CatalogEntry? {
        val offered = offered(catalog)
        if (offered.isEmpty()) return null
        return offered.filter { it.bytes <= ramBytes }.maxByOrNull { it.bytes } ?: offered.minByOrNull { it.bytes }
    }

    /**
     * Side by side, the two first runs this product offers: the largest model that fits in memory
     * (the fastest answers) and the smallest that does not (what streaming from disk is for). Empty
     * unless both exist and the memory is known.
     */
    fun pair(catalog: List<CatalogEntry>, ramBytes: Long): List<CatalogEntry> {
        if (ramBytes <= 0) return emptyList()
        val offered = offered(catalog)
        val fits = offered.filter { it.bytes <= ramBytes }.maxByOrNull { it.bytes }
        val big = offered.filter { it.bytes > ramBytes }.minByOrNull { it.bytes }
        return if (fits != null && big != null) listOf(fits, big) else emptyList()
    }

    /**
     * 3 Start when the model is on the phone or a load runs; 2 Download while it downloads, and while
     * its download is [paused] (part of it here, nothing running), unless the person asked to
     * [repick]; else 1 Pick.
     */
    fun step(state: RowState?, loading: Boolean, paused: Boolean = false, repick: Boolean = false): Int = when {
        loading || state == RowState.ON_DEVICE || state == RowState.LOADED -> 3
        state == RowState.DOWNLOADING || (paused && !repick) -> 2
        else -> 1
    }

    /** A download of the chosen model stopped part way: [partial] files here and no transfer running. */
    fun paused(partial: Boolean, state: RowState?): Boolean = partial && state != RowState.DOWNLOADING

    /** Whether the first run is the screen to open on: nothing on the phone and nothing coming. */
    fun firstRun(localModels: Int, downloading: Boolean): Boolean = localModels == 0 && !downloading
}
