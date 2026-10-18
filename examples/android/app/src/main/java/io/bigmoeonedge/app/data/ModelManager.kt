package io.bigmoeonedge.app.data

import android.content.Context
import android.os.Environment
import io.bigmoeonedge.app.BuildConfig
import java.io.File

/**
 * Where models live on this phone. Models are read in place by the bmoe-server process (a gguf can
 * be tens of GB), so every place is a real filesystem directory that process can open. The app
 * knows the places; the server says what they hold: it is started with the app's models directory
 * and each other place as a `--scan-dir`, and `GET /api/models` lists them all, split sets grouped,
 * architecture read, whether this build streams it (docs/server-api.md). Nothing here reads a gguf
 * or knows how a split set is named.
 *
 * Which other places there are depends on the distribution flavor (BuildConfig.SHARED_STORAGE):
 *
 *   always      the app-specific external files dir, so models downloaded by older builds keep
 *               working.
 *   dev only    /data/local/tmp/bmoe and the public Downloads folder, for models adb-pushed to the
 *               device. These need all-files access, which only the dev flavor requests.
 */
object ModelManager {
    // A gguf larger than the free space on shared storage cannot be copied there, but it can
    // still be adb-pushed to /data/local/tmp (same physical partition, no duplicate needed) and
    // read in place: the path is world-traversable and the file world-readable, so the app's
    // untrusted_app domain can open it.
    private val TMP_MODEL_DIR = File("/data/local/tmp/bmoe")

    /**
     * App-internal models dir — lives on /data (a real f2fs/ext4 volume), NOT the emulated/FUSE
     * external storage. That matters because O_DIRECT returns correct data here, so streamed
     * expert reads stay fast; on the emulated external dir O_DIRECT silently corrupts reads and
     * the engine has to fall back to slower buffered I/O. Downloads and imports land here, and it
     * is the server's `--models-dir`.
     */
    fun internalModelsDir(ctx: Context): File = File(ctx.filesDir, "models").apply { mkdirs() }

    /**
     * The other places a model can sit, in the order the server lists them. Order is precedence,
     * not taste: real filesystems come first, because O_DIRECT works there and the emulated dirs
     * force the engine back onto buffered I/O, and the same gguf really does sit in two places at
     * once (adb-pushed to /data/local/tmp *and* downloaded to /sdcard/Download).
     */
    fun scanDirs(ctx: Context): List<File> = buildList {
        if (BuildConfig.SHARED_STORAGE) add(TMP_MODEL_DIR)
        // The app-specific external files dir: no longer a download target (O_DIRECT is silently
        // unusable on this emulated storage), still listed so an older build's models keep working.
        ctx.getExternalFilesDir(null)?.let { add(it) }
        if (BuildConfig.SHARED_STORAGE) {
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)?.let { add(it) }
        }
    }

    /**
     * Can the app delete these files? Unlinking needs write access to the directory: an adb-pushed
     * model sits in a shell-owned one the app can read but not change.
     */
    fun canDelete(files: List<File>): Boolean = files.isNotEmpty() && files.all { it.parentFile?.canWrite() == true }

    /** Absolute path of libbmoe-server.so inside the app's nativeLibraryDir: the engine, as a process. */
    fun serverPath(ctx: Context): String =
        File(ctx.applicationInfo.nativeLibraryDir, "libbmoe-server.so").absolutePath

    /** Empty-state guidance, phrased for the current flavor's model-acquisition paths. */
    fun pushHint(): String =
        if (BuildConfig.SHARED_STORAGE) {
            "No .gguf found. Download one below, or adb-push a model to shared storage:\n" +
                "adb push model.gguf /sdcard/Download/\n" +
                "adb push model.gguf /data/local/tmp/bmoe/   (no duplicate, O_DIRECT works)"
        } else {
            "No .gguf found. Download one below, or pick a .gguf you already have on the\n" +
                "device."
        }
}
