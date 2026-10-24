package io.bigmoeonedge.app.data

import org.json.JSONObject
import java.util.Locale

/**
 * One file of a catalog entry, with the URL it is fetched from. Sizes are exact, from the repository.
 * [sha256] is the digest a finished download must have (64 lowercase hex digits), or "" when the
 * catalog names none.
 */
data class CatalogFile(val name: String, val bytes: Long, val url: String, val sha256: String = "")

/**
 * A catalog file name is a bare name: it is joined onto the models directory, so a separator, "."
 * or ".." would name a file outside it. The server refuses such a row as it loads the catalog; the
 * app holds to the same rule rather than trust that it did.
 */
fun isPlainFileName(name: String): Boolean =
    name.isNotEmpty() && name != "." && name != ".." && name.none { it == '/' || it == '\\' || it == ':' || it == '\u0000' }

/** 64 lowercase hex digits: a SHA-256 as the catalog spells it. */
fun isSha256Hex(text: String): Boolean = text.length == 64 && text.all { it in '0'..'9' || it in 'a'..'f' }

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
        /** The entry, or null when it has no files or a file this app may not write (see [isPlainFileName]). */
        fun of(o: JSONObject): CatalogEntry? {
            val files = o.optJSONArray("files")?.let { arr ->
                (0 until arr.length()).map { i ->
                    val f = arr.getJSONObject(i)
                    CatalogFile(
                        f.getString("name"), f.optLong("bytes", 0), f.optString("url"),
                        f.optString("sha256", "").lowercase(Locale.ROOT),
                    )
                }
            }.orEmpty()
            if (files.isEmpty()) return null
            if (files.any { !isPlainFileName(it.name) || (it.sha256.isNotEmpty() && !isSha256Hex(it.sha256)) }) return null
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

/**
 * Why a download ends for good on HTTP [code], or null when trying again may help. A 4xx is the
 * host's definite answer (no such file, a token it wants), except a timeout (408) and a rate limit
 * (429), which pass; a 5xx and anything else are the line or the host failing for now.
 */
fun httpRefusal(code: Int): String? =
    if (code in 400..499 && code != 408 && code != 429) "the host answered HTTP $code: the link names no file it will serve" else null

/** Why a download needing [needed] more bytes cannot start with [free] available, or null when it fits. */
fun freeSpaceRefusal(needed: Long, free: Long): String? =
    if (needed > free) "needs ${gbLabel(needed)} more, only ${gbLabel(free)} free" else null
