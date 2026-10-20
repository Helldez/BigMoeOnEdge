package io.bigmoeonedge.app.data

import org.json.JSONObject
import java.util.Locale

/** One file of a catalog entry, with the URL it is fetched from. Sizes are exact, from the repository. */
data class CatalogFile(val name: String, val bytes: Long, val url: String)

/**
 * One model the catalog offers, as `GET /api/models` lists it. The catalog is the server's data
 * file (`catalog/models.json`): this app shows it and, since a phone has no `curl` for the server
 * to run, fetches the files itself through [ModelDownloader].
 */
data class CatalogEntry(
    val id: String,
    val title: String,
    val quant: String,
    val blurb: String,
    val bytes: Long,
    /** The server's view of its own models directory: `on_disk`, `downloading`, `available`. */
    val serverStatus: String,
    val files: List<CatalogFile>,
    /** Added at run time from a repository (`POST /api/hf/add`): it can be removed; its files stay. */
    val added: Boolean = false,
    /** Bytes of it already in the models directory, complete files and `.part` files alike. */
    val received: Long = 0,
    /**
     * Not downloading on the server, and a `.part` of it is on disk: downloading resumes it. The
     * server's models directory is this app's, so its answer covers the app's own downloads too.
     */
    val partial: Boolean = false,
) {
    /** The file handed to the engine: the first (and for a split set, the first shard). */
    val fileName: String get() = files.first().name
    fun fileNames(): Set<String> = files.map { it.name }.toSet()

    companion object {
        fun of(o: JSONObject): CatalogEntry? {
            val files = o.optJSONArray("files")?.let { arr ->
                (0 until arr.length()).map { i ->
                    val f = arr.getJSONObject(i)
                    CatalogFile(f.getString("name"), f.optLong("bytes", 0), f.optString("url"))
                }
            }.orEmpty()
            if (files.isEmpty()) return null
            return CatalogEntry(
                id = o.getString("id"),
                title = o.optString("title", o.getString("id")),
                quant = o.optString("quant", ""),
                blurb = o.optString("blurb", ""),
                bytes = o.optLong("bytes", files.sumOf { it.bytes }),
                serverStatus = o.optString("status", "available"),
                files = files,
                added = o.optBoolean("added", false),
                received = o.optLong("received", 0),
                partial = o.optBoolean("partial", false),
            )
        }
    }
}

enum class CatalogStatus { ON_DEVICE, DOWNLOADING, AVAILABLE }

/**
 * The on-device rule for a catalog entry. The server answers for its own models directory, and
 * [present] is every model file it lists anywhere (the other places included); the app adds what
 * only it knows: the transfers it has in flight. A set is on the device only when every file is:
 * the first shard alone lands seconds into a multi-hour download.
 */
fun catalogStatus(e: CatalogEntry, present: Set<String>, downloading: Set<String>): CatalogStatus = when {
    e.files.any { it.name in downloading } -> CatalogStatus.DOWNLOADING
    e.serverStatus == "on_disk" || e.files.all { it.name in present } -> CatalogStatus.ON_DEVICE
    else -> CatalogStatus.AVAILABLE
}

/**
 * How this app writes a size: decimal GB, one decimal, the unit the model repositories quote.
 * Quoting a model as 17.0 GB and then refusing it for "15 GiB" reads as a bug though both are true.
 */
fun gbLabel(bytes: Long): String = String.format(Locale.US, "%.1f GB", bytes.toDouble() / 1_000_000_000L)

/**
 * A catalog row's action when part of it is already on disk, worded as the web UI words it, so the
 * same stopped download reads the same on the phone and on the desktop.
 */
fun resumeLabel(received: Long, total: Long): String =
    String.format(Locale.US, "Resume (%.1f of %s)", received.toDouble() / 1_000_000_000L, gbLabel(total))

/**
 * Bytes a download still needs on disk: what the file will hold, less what its `.part` already
 * does. `usableSpace` already counts the `.part` as used, so comparing the whole size against it
 * would refuse a resume that fits.
 */
fun bytesStillNeeded(expected: Long, onDisk: Long): Long = (expected - onDisk).coerceAtLeast(0)

/** Why a download needing [needed] more bytes cannot start with [free] available, or null when it fits. */
fun freeSpaceRefusal(needed: Long, free: Long): String? =
    if (needed > free) "needs ${gbLabel(needed)} more, only ${gbLabel(free)} free" else null
