package io.bigmoeonedge.example

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
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.coroutines.cancellation.CancellationException
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL

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

        val dir = ModelManager.internalModelsDir(applicationContext)
        val finalFile = File(dir, name)
        if (finalFile.isFile) return@withContext Result.success() // already downloaded

        val part = File(dir, name + PART_SUFFIX)
        // usableSpace already accounts for the bytes on disk in .part, so check the remainder.
        if (expected > 0 && expected - part.length() > dir.usableSpace) {
            part.delete()
            return@withContext fail("needs ${ModelCatalog.gbLabel(expected)}, only ${ModelCatalog.gbLabel(dir.usableSpace)} free")
        }

        setForeground(foregroundInfo(name, 0f))

        try {
            val total = transfer(url, part) { downloaded, size ->
                setProgressAsync(workDataOf(KEY_DONE to downloaded, KEY_TOTAL to size))
            }
            if (total >= 0 && part.length() != total) {
                // Server closed early: keep the .part so the next run's Range request resumes it.
                return@withContext retryOrFail("download interrupted at ${part.length() shr 20} MiB")
            }
            if (!part.renameTo(finalFile)) return@withContext fail("could not finalize the download")
            Result.success()
        } catch (c: CancellationException) {
            throw c // cancellation — leave the .part for a manual resume; the UI deletes it on Cancel
        } catch (t: Throwable) {
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
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            instanceFollowRedirects = true
            connectTimeout = 30_000
            readTimeout = 30_000
            // Defeat gzip: a re-encoded body would make the byte offsets in a Range resume wrong.
            setRequestProperty("Accept-Encoding", "identity")
            if (from > 0) setRequestProperty("Range", "bytes=$from-")
        }
        try {
            val code = conn.responseCode
            when (code) {
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

    private fun retryOrFail(message: String): Result =
        if (runAttemptCount < MAX_ATTEMPTS) Result.retry() else fail(message)

    private fun fail(message: String): Result = Result.failure(workDataOf(KEY_ERROR to message))

    private fun foregroundInfo(name: String, frac: Float): ForegroundInfo {
        val nm = applicationContext.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL, "Model downloads", NotificationManager.IMPORTANCE_LOW)
            )
        }
        val notif: Notification = Notification.Builder(applicationContext, CHANNEL)
            .setContentTitle("Downloading model")
            .setContentText(name)
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
