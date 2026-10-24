package io.bigmoeonedge.app.data

import io.bigmoeonedge.app.engine.LocalModel
import io.bigmoeonedge.app.engine.ModelsList

/**
 * The Models screen's one list (docs/screens.md, "Models"): the files the server lists on this
 * phone and the catalog's entries, merged by file, each row in one state with one primary action.
 * Everything a row says comes from `GET /api/models` and the app's own transfers in flight: no
 * model, size or architecture is named here. Same rules as the web UI (ui/src/lib/library.ts),
 * with the downloads the app's rather than the server's.
 */
enum class RowState(val label: String) {
    /** In the catalog, nothing of it here yet: Download. */
    AVAILABLE("Available"),
    /** Its transfer runs: progress and Cancel. */
    DOWNLOADING("Downloading"),
    /** A stopped download left partial files: Resume, or Delete partial. */
    PARTIAL("Partly downloaded"),
    /** On the phone, ready to load. */
    ON_DEVICE("On this device"),
    /** The model the engine has open: Reload, Unload, Go to Chat. */
    LOADED("Loaded"),
    /** A split set with shards missing: not loadable; an entry's download fetches the rest. */
    INCOMPLETE("Shards missing"),
}

data class LibraryRow(
    /** Stable across refreshes: the local path, or the catalog id for an entry with nothing here. */
    val key: String,
    val title: String,
    val local: LocalModel?,
    val entry: CatalogEntry?,
    val state: RowState,
    /** The catalog's suggestion, nothing of it here and nothing started: listed after the others. */
    val suggested: Boolean,
    /** The listing's own name for it (`folder/name`) when it sits in the server's models directory,
     *  which the server deletes; null elsewhere (a scan directory, or nothing on disk). */
    val deleteName: String?,
)

/**
 * The merged list: one row per model on the phone (the catalog entry that made it folded in), then
 * the entries whose download has begun, then the suggestions. [local] is the models alone, the
 * companion files already split off ([FileRole.split]); [downloading] the file names the app's
 * transfers have in flight.
 */
fun libraryRows(models: ModelsList, local: List<LocalModel>, downloading: Set<String>, loadedPath: String?): List<LibraryRow> {
    val rows = mutableListOf<LibraryRow>()
    val claimed = mutableSetOf<String>()
    fun inFlight(e: CatalogEntry) = e.files.any { it.name in downloading }
    for (m in local) {
        // The entry a download made: its first file, by name, at the top of a directory (the server
        // lists the models directory first, so a copy there wins over one pushed elsewhere).
        val entry = models.catalog.firstOrNull { it.id !in claimed && m.folder.isEmpty() && m.name == it.fileName }
        entry?.let { claimed += it.id }
        val state = when {
            m.path == loadedPath -> RowState.LOADED
            entry != null && inFlight(entry) -> RowState.DOWNLOADING
            !m.complete -> if (entry?.partial == true) RowState.PARTIAL else RowState.INCOMPLETE
            else -> RowState.ON_DEVICE
        }
        rows += LibraryRow(
            key = m.path,
            title = entry?.title ?: m.name,
            local = m,
            entry = entry,
            state = state,
            suggested = false,
            deleteName = if (m.dir == models.modelsDir) (if (m.folder.isNotEmpty()) "${m.folder}/${m.name}" else m.name) else null,
        )
    }
    val present = models.fileNames
    val started = mutableListOf<LibraryRow>()
    val suggested = mutableListOf<LibraryRow>()
    for (e in models.catalog) {
        if (e.id in claimed) continue
        val state = when (catalogStatus(e, present, downloading)) {
            CatalogStatus.DOWNLOADING -> RowState.DOWNLOADING
            CatalogStatus.ON_DEVICE -> RowState.ON_DEVICE
            CatalogStatus.AVAILABLE -> if (e.partial) RowState.PARTIAL else RowState.AVAILABLE
        }
        val row = LibraryRow("catalog:${e.id}", e.title, null, e, state, state == RowState.AVAILABLE && !e.added, null)
        if (row.suggested) suggested += row else started += row
    }
    return rows + started + suggested
}

/** The running downloads at a glance, for the status line: how many, and the share done where sizes are known. */
data class DownloadSummary(val count: Int, val fraction: Double?) {
    /** "Downloading 42%", or "2 downloads 42%"; no share while no size is known. */
    val text: String
        get() = (if (count == 1) "Downloading" else "$count downloads") +
            (fraction?.let { " ${(it * 100).toInt()}%" } ?: "")
}

/** Over [transfers], each (bytes received, bytes in all; 0 or less when unknown); null when none runs. */
fun downloadSummary(transfers: List<Pair<Long, Long>>): DownloadSummary? {
    if (transfers.isEmpty()) return null
    val known = transfers.filter { it.second > 0 }
    val total = known.sumOf { it.second }
    val received = known.sumOf { minOf(it.first, it.second) }
    return DownloadSummary(transfers.size, if (total > 0) received.toDouble() / total else null)
}

/**
 * The transfers in flight as the status line counts them: one per catalog entry (a split set's
 * shards are one download), one per file the app fetches on its own. [entryProgress] gives an
 * entry's progress over its whole set.
 */
fun runningTransfers(
    catalog: List<CatalogEntry>,
    live: Map<String, ModelDownloader.Progress>,
    entryProgress: (CatalogEntry) -> ModelDownloader.Progress?,
): List<Pair<Long, Long>> {
    val names = catalog.flatMap { it.fileNames() }.toSet()
    val entries = catalog.mapNotNull { e -> entryProgress(e)?.let { it.downloadedBytes to it.totalBytes } }
    val strays = live.filterKeys { it !in names }.values.map { it.downloadedBytes to it.totalBytes }
    return entries + strays
}

/** A verdict's colour, from the code the server sent (`verdict.fit`); the words are the server's. */
enum class Tone { OK, WARN, ERR, NONE }

/** [fit] picks the colour; an older server that sends none is read by its [mode], the next best thing. */
fun fitTone(fit: String, mode: String = ""): Tone = when (fit) {
    "fits" -> Tone.OK
    "no_streaming" -> Tone.WARN
    "will_not_fit", "no_disk", "not_a_model" -> Tone.ERR
    "" -> when (mode) {
        "" -> Tone.NONE
        "streams" -> Tone.OK
        "not readable", "not a model" -> Tone.ERR
        else -> Tone.WARN
    }
    else -> Tone.NONE
}

/** The four steps of a model from Hugging Face, and where the person is: what they have, not what they last pressed. */
val HF_STEPS = listOf("Find", "Pick", "Check", "Download")

fun hfStep(listed: Boolean, chosen: Boolean, checked: Boolean, added: Boolean): Int = when {
    added || (checked && chosen) -> 3
    chosen -> 2
    listed -> 1
    else -> 0
}
