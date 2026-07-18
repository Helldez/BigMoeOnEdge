package io.bigmoeonedge.example

import android.content.Context
import android.net.Uri
import androidx.work.BackoffPolicy
import androidx.work.Constraints
import androidx.work.ExistingWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.WorkInfo
import androidx.work.WorkManager
import androidx.work.workDataOf
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * Starts and tracks model downloads. The transfer itself runs in [DownloadWorker] (an in-app
 * HTTP download into the O_DIRECT-capable internal storage — see that class for why the system
 * DownloadManager can't be used here); this object is the thin facade the UI drives.
 *
 * A download is identified by its filename, which is also its unique-work name: enqueuing the
 * same model twice is a no-op, and an in-flight transfer is re-discoverable after process death
 * because WorkManager persists it.
 */
object ModelDownloader {
    private const val NAME_TAG_PREFIX = "name:"

    enum class State { PENDING, RUNNING, PAUSED, SUCCESS, FAILED }

    data class Progress(
        val id: String, // the unique-work name == the filename
        val name: String,
        val downloadedBytes: Long,
        val totalBytes: Long, // -1 when the server didn't send a length
        val state: State,
        val reason: String? = null,
    )

    /**
     * Start a download. Returns the unique-work id (the filename), or an error if the URL doesn't
     * name a .gguf file or the model cannot fit. No model names or hosts are assumed — for a pasted
     * URL the filename comes from the URL itself.
     *
     * [fileName] overrides that for catalog downloads, where the on-disk name is known upfront and
     * must not depend on redirects or query strings. [expectedBytes] (when > 0) is checked against
     * free space before enqueuing so the user isn't left waiting for a transfer that can't fit.
     */
    fun enqueue(
        ctx: Context,
        rawUrl: String,
        fileName: String? = null,
        expectedBytes: Long = -1L,
    ): Result<String> = runCatching {
        val url = rawUrl.trim()
        val uri = Uri.parse(url)
        require(uri.scheme == "http" || uri.scheme == "https") { "URL must be http(s)" }

        val name = fileName ?: DownloadWorker.fileNameFromUrl(uri)
        require(name.endsWith(".gguf")) { "URL must point to a .gguf file" }

        val dir = ModelManager.internalModelsDir(ctx)
        if (expectedBytes > 0 && expectedBytes > dir.usableSpace) {
            error(
                "needs ${ModelCatalog.gbLabel(expectedBytes)}, " +
                    "only ${ModelCatalog.gbLabel(dir.usableSpace)} free"
            )
        }

        val req = OneTimeWorkRequestBuilder<DownloadWorker>()
            .setInputData(
                workDataOf(
                    DownloadWorker.KEY_URL to url,
                    DownloadWorker.KEY_NAME to name,
                    DownloadWorker.KEY_EXPECTED to expectedBytes,
                )
            )
            .addTag(DownloadWorker.TAG)
            .addTag(NAME_TAG_PREFIX + name)
            .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build())
            .setBackoffCriteria(BackoffPolicy.EXPONENTIAL, 30, TimeUnit.SECONDS)
            .build()
        // Block until the work is persisted so the caller's immediate activeDownloads() reseed
        // sees it (a fast local DB write) — otherwise the row wouldn't show as downloading.
        WorkManager.getInstance(ctx).enqueueUniqueWork(name, ExistingWorkPolicy.KEEP, req).result.get()
        name
    }

    /** Current status of a download, or null if it is unknown, cancelled, or already cleared. */
    suspend fun query(ctx: Context, name: String): Progress? = withContext(Dispatchers.IO) {
        runCatching {
            val info = WorkManager.getInstance(ctx).getWorkInfosForUniqueWork(name).get().lastOrNull()
                ?: return@runCatching null
            when (info.state) {
                WorkInfo.State.ENQUEUED, WorkInfo.State.BLOCKED ->
                    Progress(name, name, 0, -1, State.PENDING)
                WorkInfo.State.RUNNING -> Progress(
                    name, name,
                    info.progress.getLong(DownloadWorker.KEY_DONE, 0),
                    info.progress.getLong(DownloadWorker.KEY_TOTAL, -1),
                    State.RUNNING,
                )
                WorkInfo.State.SUCCEEDED -> Progress(name, name, 0, 0, State.SUCCESS)
                WorkInfo.State.FAILED -> Progress(
                    name, name, 0, -1, State.FAILED,
                    info.outputData.getString(DownloadWorker.KEY_ERROR) ?: "download failed",
                )
                WorkInfo.State.CANCELLED -> null // user cancelled: no error to surface
            }
        }.getOrNull()
    }

    /**
     * Model downloads still in flight, as filename -> id (both the filename). WorkManager persists
     * work across process death, so a download started before the app was killed is picked back up
     * here instead of running unseen. Never throws — an empty map costs a progress bar, not the
     * whole screen.
     */
    fun activeDownloads(ctx: Context): Map<String, String> = runCatching {
        WorkManager.getInstance(ctx).getWorkInfosByTag(DownloadWorker.TAG).get()
            .filter { !it.state.isFinished }
            .mapNotNull { info -> info.tags.firstOrNull { it.startsWith(NAME_TAG_PREFIX) }?.removePrefix(NAME_TAG_PREFIX) }
            .associateWith { it }
    }.getOrDefault(emptyMap())

    /**
     * Finish a successful download by renaming `<name>.gguf.part` to `<name>.gguf`. The worker
     * already does this on success, so this is an idempotent safety net: if the final file exists
     * it is returned as-is; otherwise a leftover .part is renamed.
     */
    fun finalizeDownload(ctx: Context, name: String): File? {
        val dir = ModelManager.internalModelsDir(ctx)
        val finalFile = File(dir, name)
        if (finalFile.isFile) return finalFile
        val part = File(dir, name + DownloadWorker.PART_SUFFIX)
        if (!part.isFile) return null
        return if (part.renameTo(finalFile)) finalFile else null
    }

    /** Cancel a download and delete its leftover .part file. */
    fun cancel(ctx: Context, name: String) {
        // Block until the cancellation is persisted, so a caller's immediate activeDownloads()
        // reseed sees the work as finished instead of racing it back in as still-active.
        WorkManager.getInstance(ctx).cancelUniqueWork(name).result.get()
        File(ModelManager.internalModelsDir(ctx), name + DownloadWorker.PART_SUFFIX).delete()
    }
}
