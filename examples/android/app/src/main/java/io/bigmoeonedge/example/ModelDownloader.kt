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
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID
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

    /** What [events] reports. A download ends in exactly one of [Completed] or [Failed]. */
    sealed interface Event {
        /** Every transfer still in flight, by filename. Re-emitted on each WorkManager update. */
        data class InFlight(val downloads: Map<String, Progress>) : Event

        /** [name] landed and is finalized on disk — the caller should re-scan. */
        data class Completed(val name: String) : Event

        data class Failed(val name: String, val reason: String) : Event
    }

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
        // Block until the work is persisted (a fast local DB write) so the next events() emission
        // already carries it — otherwise the row wouldn't show as downloading.
        WorkManager.getInstance(ctx).enqueueUniqueWork(name, ExistingWorkPolicy.KEEP, req).result.get()
        name
    }

    /**
     * Every download's progress and outcome, as a stream. WorkManager's own flow drives it, so the
     * UI observes instead of polling, and a transfer started before the app was killed reappears on
     * the first emission rather than running unseen.
     *
     * Finalization lives here, not in the observer: a landed download is renamed .part -> .gguf
     * before [Event.Completed] is emitted, so by the time anyone re-scans the model is on disk under
     * its final name.
     *
     * Work that was ALREADY terminal when collection started is finalized but reported silently: on
     * a fresh launch WorkManager still holds the last run's succeeded rows, and replaying them would
     * fire a spurious re-scan for a model the startup scan has already found.
     */
    fun events(ctx: Context): Flow<Event> = flow {
        // Terminal work already accounted for, keyed by work id rather than filename: WorkManager
        // re-emits a finished row on every update, but re-downloading the same model (deleted, then
        // fetched again) is NEW work with a new id and must still report its own completion.
        val settled = mutableSetOf<UUID>()
        var primed = false // has the first (catch-up) emission been processed?
        WorkManager.getInstance(ctx).getWorkInfosByTagFlow(DownloadWorker.TAG).collect { infos ->
            val live = mutableMapOf<String, Progress>()
            val outcomes = mutableListOf<Event>()
            for (info in infos) {
                val name = info.tags.firstOrNull { it.startsWith(NAME_TAG_PREFIX) }
                    ?.removePrefix(NAME_TAG_PREFIX) ?: continue
                when (info.state) {
                    WorkInfo.State.ENQUEUED, WorkInfo.State.BLOCKED ->
                        live[name] = Progress(name, name, 0, -1, State.PENDING)
                    WorkInfo.State.RUNNING -> live[name] = Progress(
                        name, name,
                        info.progress.getLong(DownloadWorker.KEY_DONE, 0),
                        info.progress.getLong(DownloadWorker.KEY_TOTAL, -1),
                        State.RUNNING,
                    )
                    WorkInfo.State.SUCCEEDED -> if (settled.add(info.id)) {
                        withContext(Dispatchers.IO) { finalizeDownload(ctx, name) }
                        if (primed) outcomes += Event.Completed(name)
                    }
                    WorkInfo.State.FAILED -> if (settled.add(info.id) && primed) {
                        outcomes += Event.Failed(
                            name,
                            info.outputData.getString(DownloadWorker.KEY_ERROR) ?: "download failed",
                        )
                    }
                    WorkInfo.State.CANCELLED -> settled.add(info.id) // user cancelled: no error to surface
                }
            }
            primed = true
            outcomes.forEach { emit(it) }
            emit(Event.InFlight(live))
        }
    }

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
        // Block until the cancellation is persisted, so the next events() emission reports the work
        // as finished instead of racing it back in as still-active.
        WorkManager.getInstance(ctx).cancelUniqueWork(name).result.get()
        File(ModelManager.internalModelsDir(ctx), name + DownloadWorker.PART_SUFFIX).delete()
    }
}
