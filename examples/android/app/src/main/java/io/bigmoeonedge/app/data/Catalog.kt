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
