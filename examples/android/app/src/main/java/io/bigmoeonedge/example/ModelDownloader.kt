package io.bigmoeonedge.example

import android.app.DownloadManager
import android.content.Context
import android.net.Uri
import java.io.File

/**
 * Downloads a gguf by URL into the app-specific external files dir using the system
 * [DownloadManager]. That destination needs no storage permission, and DownloadManager keeps
 * going across process death and resumes on reconnect — the right fit for multi-GB models.
 *
 * The file is written as `<name>.gguf.part` and renamed to `<name>.gguf` only on success, so a
 * half-finished download is never listed as a runnable model.
 */
object ModelDownloader {
    private const val PART_SUFFIX = ".part"

    enum class State { PENDING, RUNNING, PAUSED, SUCCESS, FAILED }

    data class Progress(
        val id: Long,
        val name: String,
        val downloadedBytes: Long,
        val totalBytes: Long, // -1 when the server didn't send a length
        val state: State,
        val reason: String? = null,
    )

    /**
     * Start a download. Returns the DownloadManager id, or an error if the URL doesn't name a
     * .gguf file. No model names or hosts are assumed — the filename comes from the URL.
     */
    fun enqueue(ctx: Context, rawUrl: String): Result<Long> = runCatching {
        val url = rawUrl.trim()
        val uri = Uri.parse(url)
        require(uri.scheme == "http" || uri.scheme == "https") { "URL must be http(s)" }

        val name = fileNameFromUrl(uri)
        require(name.endsWith(".gguf")) { "URL must point to a .gguf file" }

        val dm = ctx.getSystemService(Context.DOWNLOAD_SERVICE) as DownloadManager
        val req = DownloadManager.Request(uri)
            .setTitle(name)
            .setDescription("BigMoeOnEdge model")
            .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
            .setAllowedOverMetered(true)
            .setAllowedOverRoaming(false)
            // App-specific external files dir → no permission required, and the same directory
            // ModelManager scans. Written as .part so the scanner ignores it until complete.
            .setDestinationInExternalFilesDir(ctx, null, name + PART_SUFFIX)
        dm.enqueue(req)
    }

    /** Current status of a download, or null if the id is unknown. */
    fun query(ctx: Context, id: Long): Progress? {
        val dm = ctx.getSystemService(Context.DOWNLOAD_SERVICE) as DownloadManager
        dm.query(DownloadManager.Query().setFilterById(id)).use { c ->
            if (c == null || !c.moveToFirst()) return null
            val status = c.getInt(c.getColumnIndexOrThrow(DownloadManager.COLUMN_STATUS))
            val soFar = c.getLong(c.getColumnIndexOrThrow(DownloadManager.COLUMN_BYTES_DOWNLOADED_SO_FAR))
            val total = c.getLong(c.getColumnIndexOrThrow(DownloadManager.COLUMN_TOTAL_SIZE_BYTES))
            val title = c.getString(c.getColumnIndexOrThrow(DownloadManager.COLUMN_TITLE)) ?: "model.gguf"
            val reasonCode = c.getInt(c.getColumnIndexOrThrow(DownloadManager.COLUMN_REASON))
            val state = when (status) {
                DownloadManager.STATUS_PENDING -> State.PENDING
                DownloadManager.STATUS_RUNNING -> State.RUNNING
                DownloadManager.STATUS_PAUSED -> State.PAUSED
                DownloadManager.STATUS_SUCCESSFUL -> State.SUCCESS
                else -> State.FAILED
            }
            val reason = when (state) {
                State.FAILED -> failureReason(reasonCode)
                State.PAUSED -> pauseReason(reasonCode)
                else -> null
            }
            return Progress(id, title, soFar, total, state, reason)
        }
    }

    /**
     * Finish a successful download: rename `<name>.gguf.part` to `<name>.gguf` in the app models
     * dir and return the final file, or null if the part file is missing. Idempotent — if the
     * final file already exists it is returned as-is.
     */
    fun finalizeDownload(ctx: Context, name: String): File? {
        val dir = ModelManager.appModelsDir(ctx) ?: return null
        val finalFile = File(dir, name)
        if (finalFile.isFile) return finalFile
        val part = File(dir, name + PART_SUFFIX)
        if (!part.isFile) return null
        return if (part.renameTo(finalFile)) finalFile else null
    }

    /** Remove a download from DownloadManager and delete any leftover .part file. */
    fun cancel(ctx: Context, id: Long, name: String) {
        val dm = ctx.getSystemService(Context.DOWNLOAD_SERVICE) as DownloadManager
        dm.remove(id)
        ModelManager.appModelsDir(ctx)?.let { File(it, name + PART_SUFFIX).delete() }
    }

    // Last path segment, query stripped, sanitized to a bare filename (no separators).
    private fun fileNameFromUrl(uri: Uri): String {
        val seg = uri.lastPathSegment?.substringAfterLast('/') ?: ""
        val cleaned = seg.substringBefore('?').filter { it.isLetterOrDigit() || it in "._-" }
        require(cleaned.isNotEmpty()) { "cannot derive a filename from the URL" }
        return cleaned
    }

    private fun failureReason(code: Int): String = when (code) {
        DownloadManager.ERROR_INSUFFICIENT_SPACE -> "not enough free space"
        DownloadManager.ERROR_HTTP_DATA_ERROR -> "network data error"
        DownloadManager.ERROR_UNHANDLED_HTTP_CODE -> "server returned an unexpected status"
        DownloadManager.ERROR_FILE_ERROR -> "file error writing the download"
        DownloadManager.ERROR_CANNOT_RESUME -> "download could not resume"
        else -> "download failed (code $code)"
    }

    private fun pauseReason(code: Int): String = when (code) {
        DownloadManager.PAUSED_WAITING_FOR_NETWORK -> "waiting for network"
        DownloadManager.PAUSED_WAITING_TO_RETRY -> "waiting to retry"
        DownloadManager.PAUSED_QUEUED_FOR_WIFI -> "queued for Wi-Fi"
        else -> "paused"
    }
}
