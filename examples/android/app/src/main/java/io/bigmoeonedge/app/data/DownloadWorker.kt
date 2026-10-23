package io.bigmoeonedge.app.data

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.Build
import androidx.work.CoroutineWorker
import androidx.work.ForegroundInfo
import androidx.work.WorkerParameters
import androidx.work.workDataOf
import io.bigmoeonedge.app.engine.DOWNLOAD_NETWORK_DENIED
import io.bigmoeonedge.app.engine.isNetworkDenied
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.coroutines.cancellation.CancellationException
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * Downloads a gguf over HTTP straight into the app-internal models dir (real f2fs/ext4, where
 * O_DIRECT works — see [ModelManager.internalModelsDir]). This is why the app does the transfer
 * itself instead of using the system DownloadManager, which can only write to the emulated
 * external storage where O_DIRECT is silently unusable and the engine falls back to slow
 * buffered I/O.
 *
 * The bytes go to `<name>.gguf.part` and the file is renamed to `<name>.gguf` only once complete,
 * so a half-finished download is never listed as a runnable model. A `Range` request resumes an
 * interrupted `.part` instead of restarting a multi-GB transfer, and the worker runs as a
 * foreground service so a long download survives the app going to the background.
 */
class DownloadWorker(ctx: Context, params: WorkerParameters) : CoroutineWorker(ctx, params) {

    override suspend fun doWork(): Result = withContext(Dispatchers.IO) {
        val url = inputData.getString(KEY_URL) ?: return@withContext fail("missing URL")
        val name = inputData.getString(KEY_NAME) ?: return@withContext fail("missing filename")
        val expected = inputData.getLong(KEY_EXPECTED, -1L)
        val sha256 = inputData.getString(KEY_SHA256).orEmpty()
        if (!isPlainFileName(name)) return@withContext fail("'$name' is not a plain file name")

        val dir = ModelManager.internalModelsDir(applicationContext)
        val finalFile = File(dir, name)
        if (finalFile.isFile) return@withContext Result.success() // already downloaded

        val part = File(dir, name + PART_SUFFIX)
        // usableSpace already accounts for the bytes on disk in .part, so check the remainder. The
        // .part stays: freeing space and pressing Resume continues it.
        if (expected > 0) {
            freeSpaceRefusal(bytesStillNeeded(expected, part.length()), dir.usableSpace)?.let {
                return@withContext fail(it)
            }
        }

        setForeground(foregroundInfo(name, part.length(), expected))

        try {
            val total = transfer(url, part) { downloaded, size ->
                setProgressAsync(workDataOf(KEY_DONE to downloaded, KEY_TOTAL to size))
                // The notification follows the same throttled ticks as the in-app row: it is what the
                // user sees of a long download with the app in the background.
                setForegroundAsync(foregroundInfo(name, downloaded, if (size > 0) size else expected))
            }
            if (total >= 0 && part.length() != total) {
                // Server closed early: keep the .part so the next run's Range request resumes it.
                return@withContext retryOrFail("download interrupted at ${part.length() shr 20} MiB")
            }
            if (sha256.isNotEmpty()) {
                setForegroundAsync(foregroundInfo(name, part.length(), expected, "Checking model"))
                val actual = sha256Of(part)
                if (actual != sha256) {
                    // Resuming would only reproduce the same bytes: the part goes, and a retry starts over.
                    part.delete()
                    return@withContext fail(
                        "the file's SHA-256 is ${actual.take(12)}..., the catalog names ${sha256.take(12)}...: " +
                            "it is not the file the catalog lists; the download is deleted"
                    )
                }
            }
            if (!part.renameTo(finalFile)) return@withContext fail("could not finalize the download")
            Result.success()
        } catch (c: CancellationException) {
            throw c // cancellation — the .part stays, and the row offers Resume and Delete partial download
        } catch (t: Throwable) {
            // Denied, not failing: no back-off brings the network back, so say where the switch is.
            if (isNetworkDenied(t)) return@withContext fail(DOWNLOAD_NETWORK_DENIED)
            // Transient network error: keep the .part and let WorkManager back off and resume.
            retryOrFail(t.message ?: "network error")
        }
    }

    /**
     * Stream the URL into [part], resuming from its current length with a `Range` request.
     * Returns the expected total size in bytes, or -1 if the server didn't report one.
     */
    private suspend fun transfer(url: String, part: File, onProgress: (Long, Long) -> Unit): Long {
        var from = if (part.isFile) part.length() else 0L
        val conn = openWithRange(url, from)
        try {
            when (val code = conn.responseCode) {
                HttpURLConnection.HTTP_PARTIAL -> { /* 206: server honored the resume */ }
                HttpURLConnection.HTTP_OK -> from = 0L // 200: no resume, start the .part over
                416 -> return part.length() // range not satisfiable: .part is already the whole file
                else -> throw java.io.IOException("server returned HTTP $code")
            }
            // Total = bytes already on disk + what this response will deliver.
            val remaining = conn.contentLengthLong
            val total = if (remaining >= 0) from + remaining else -1L

            RandomAccessFile(part, "rw").use { out ->
                out.seek(from)
                if (from == 0L) out.setLength(0L)
                conn.inputStream.use { src ->
                    val buf = ByteArray(1 shl 20)
                    var done = from
                    var lastTick = 0L
                    onProgress(done, total)
                    while (true) {
                        if (isStopped) throw CancellationException("cancelled")
                        val n = src.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        done += n
                        // Throttle UI/DB writes: progress Data every ~500 ms, not every 1 MiB chunk.
                        val now = System.currentTimeMillis()
                        if (now - lastTick >= 500) {
                            onProgress(done, total)
                            lastTick = now
                        }
                    }
                    onProgress(done, total)
                }
            }
            return total
        } finally {
            conn.disconnect()
        }
    }

    /**
     * Open [startUrl] following redirects MANUALLY, re-applying the Range and identity-encoding
     * headers on every hop. HttpURLConnection's automatic redirect (instanceFollowRedirects) drops
     * request headers across a cross-host 3xx — which is exactly Hugging Face's resolve → CDN
     * redirect — so a resume would silently lose its Range header, get a 200, and restart from zero.
     */
    private fun openWithRange(startUrl: String, from: Long): HttpURLConnection {
        var url = startUrl
        var hops = 0
        while (true) {
            val conn = (URL(url).openConnection() as HttpURLConnection).apply {
                instanceFollowRedirects = false
                connectTimeout = 30_000
                readTimeout = 30_000
                // Defeat gzip: a re-encoded body would make the byte offsets in a Range resume wrong.
                setRequestProperty("Accept-Encoding", "identity")
                if (from > 0) setRequestProperty("Range", "bytes=$from-")
            }
            when (conn.responseCode) {
                301, 302, 303, 307, 308 -> {
                    val loc = conn.getHeaderField("Location")
                    conn.disconnect()
                    if (loc.isNullOrBlank() || ++hops > 5) throw java.io.IOException("too many redirects")
                    url = URL(URL(url), loc).toString() // resolve a possibly-relative Location
                }
                else -> return conn
            }
        }
    }

    /** The file's SHA-256 as 64 lowercase hex digits. Stops, throwing, when the work is cancelled. */
    private fun sha256Of(file: File): String {
        val md = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { src ->
            val buf = ByteArray(1 shl 20)
            while (true) {
                if (isStopped) throw CancellationException("cancelled")
                val n = src.read(buf)
                if (n < 0) break
                md.update(buf, 0, n)
            }
        }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    private fun retryOrFail(message: String): Result =
        if (runAttemptCount < MAX_ATTEMPTS) Result.retry() else fail(message)

    private fun fail(message: String): Result = Result.failure(workDataOf(KEY_ERROR to message))

    private fun foregroundInfo(
        name: String,
        done: Long,
        total: Long,
        title: String = "Downloading model",
    ): ForegroundInfo {
        val frac = if (total > 0) done.toFloat() / total else -1f
        val nm = applicationContext.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL, "Model downloads", NotificationManager.IMPORTANCE_LOW)
            )
        }
        val notif: Notification = Notification.Builder(applicationContext, CHANNEL)
            .setContentTitle(title)
            .setContentText(if (total > 0) "$name · ${gbLabel(done)} of ${gbLabel(total)}" else name)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setOngoing(true)
            .setProgress(100, (frac * 100).toInt().coerceIn(0, 100), frac < 0f)
            .build()
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            ForegroundInfo(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            ForegroundInfo(NOTIF_ID, notif)
        }
    }

    companion object {
        const val PART_SUFFIX = ".part"
        const val TAG = "model-download"

        const val KEY_URL = "url"
        const val KEY_NAME = "name"
        const val KEY_EXPECTED = "expected"
        const val KEY_SHA256 = "sha256"
        const val KEY_DONE = "done"
        const val KEY_TOTAL = "total"
        const val KEY_ERROR = "error"

        private const val CHANNEL = "download"
        private const val NOTIF_ID = 42
        private const val MAX_ATTEMPTS = 5

        /** Last path segment of a URL, query stripped, sanitized to a bare filename. */
        fun fileNameFromUrl(uri: Uri): String {
            val seg = uri.lastPathSegment?.substringAfterLast('/') ?: ""
            val cleaned = seg.substringBefore('?').filter { it.isLetterOrDigit() || it in "._-" }
            require(cleaned.isNotEmpty()) { "cannot derive a filename from the URL" }
            return cleaned
        }
    }
}
