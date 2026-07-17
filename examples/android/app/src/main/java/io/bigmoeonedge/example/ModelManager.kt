package io.bigmoeonedge.example

import android.content.Context
import android.os.Environment
import java.io.File

/**
 * Finds MoE gguf models on device. Models are read in place by the bmoe-cli subprocess (a gguf
 * can be tens of GB), so every source is a real filesystem directory the subprocess can open.
 *
 * Which directories are scanned depends on the distribution flavor (BuildConfig.SHARED_STORAGE):
 *
 *   always      the app-specific external files dir — no permission needed. The in-app URL
 *               downloader and the file picker both land models here.
 *   dev only    the public Downloads folder and /data/local/tmp/bmoe, for models adb-pushed
 *               to the device. These need all-files access, which only the dev flavor requests.
 *
 * Only MoE models are listed: this engine streams experts, so dense models are filtered out by
 * reading the gguf header (see [GgufHeader]).
 */
object ModelManager {
    // A gguf larger than the free space on shared storage cannot be copied there, but it can
    // still be adb-pushed to /data/local/tmp (same physical partition, no duplicate needed) and
    // read in place: the path is world-traversable and the file world-readable, so the app's
    // untrusted_app domain can open it.
    private val TMP_MODEL_DIR = File("/data/local/tmp/bmoe")

    /** The app-specific external files dir — the download target, readable with no permission. */
    fun appModelsDir(ctx: Context): File? = ctx.getExternalFilesDir(null)

    /**
     * App-internal models dir — lives on /data (a real f2fs/ext4 volume), NOT the emulated/FUSE
     * external storage. That matters because O_DIRECT returns correct data here, so streamed
     * expert reads stay fast; on the emulated external dir O_DIRECT silently corrupts reads and
     * the engine has to fall back to slower buffered I/O. Imports land here for that reason.
     */
    fun internalModelsDir(ctx: Context): File = File(ctx.filesDir, "models").apply { mkdirs() }

    private fun scanDirs(ctx: Context): List<File> = buildList {
        add(internalModelsDir(ctx))
        appModelsDir(ctx)?.let { add(it) }
        if (BuildConfig.SHARED_STORAGE) {
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)?.let { add(it) }
            if (TMP_MODEL_DIR.isDirectory) add(TMP_MODEL_DIR)
        }
    }

    private fun allGguf(ctx: Context): List<File> =
        scanDirs(ctx)
            .flatMap { it.listFiles { f -> f.isFile && f.name.endsWith(".gguf") }?.toList() ?: emptyList() }
            .distinctBy { it.absolutePath }
            .sortedBy { it.name }

    /** List MoE models only. Blocking header reads — call off the main thread. */
    fun listMoeModels(ctx: Context): List<File> = allGguf(ctx).filter { GgufHeader.isMoe(it) }

    /** Absolute path of libbmoe-cli.so inside the app's nativeLibraryDir. */
    fun cliPath(ctx: Context): String =
        File(ctx.applicationInfo.nativeLibraryDir, "libbmoe-cli.so").absolutePath

    /** Empty-state guidance, phrased for the current flavor's model-acquisition paths. */
    fun pushHint(): String =
        if (BuildConfig.SHARED_STORAGE) {
            "No MoE .gguf found. Add one below (download by URL or pick a file), or adb-push a\n" +
                "model to shared storage:\n" +
                "adb push model.gguf /sdcard/Download/"
        } else {
            "No MoE .gguf found. Add one below: download by URL, or pick a .gguf you already\n" +
                "have on the device."
        }
}
