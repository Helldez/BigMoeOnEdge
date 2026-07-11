package io.bigmoeonedge.example

import android.content.Context
import android.os.Environment
import java.io.File

/**
 * Finds MoE gguf models on shared storage. A gguf can be tens of GB, so the app reads it
 * in place (via the bmoe-cli subprocess) rather than copying it into app storage — which
 * requires all-files access. Scans the public Downloads folder plus the app's own
 * external files dir.
 *
 *   adb push Qwen3-30B-A3B-Q4_K_M.gguf /sdcard/Download/
 *
 * Only MoE models are listed: this engine streams experts, so dense models (which have no
 * expert tensors) are filtered out by probing the gguf header.
 */
object ModelManager {
    // Every MoE gguf, in any expert layout, names its down projection blk.<il>.ffn_down_exps
    // — a marker absent from dense models (which have blk.<il>.ffn_down). It lives in the
    // tensor-info block right after the KV metadata, well within this probe window.
    private val MOE_MARKER = "ffn_down_exps".toByteArray(Charsets.US_ASCII)
    private const val PROBE_BYTES = 16 * 1024 * 1024

    // A gguf larger than the free space on shared storage cannot be copied there, but it can
    // still be adb-pushed to /data/local/tmp (same physical partition, no duplicate needed) and
    // read in place: the path is world-traversable and the file world-readable, so the app's
    // untrusted_app domain can open it. Scanned in addition to shared storage for that case.
    private val TMP_MODEL_DIR = File("/data/local/tmp/shardllm")

    private fun scanDirs(ctx: Context): List<File> = buildList {
        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)?.let { add(it) }
        ctx.getExternalFilesDir(null)?.let { add(it) }
        if (TMP_MODEL_DIR.isDirectory) add(TMP_MODEL_DIR)
    }

    private fun allGguf(ctx: Context): List<File> =
        scanDirs(ctx)
            .flatMap { it.listFiles { f -> f.isFile && f.name.endsWith(".gguf") }?.toList() ?: emptyList() }
            .distinctBy { it.absolutePath }
            .sortedBy { it.name }

    /** True if the gguf declares expert tensors (i.e. it is a MoE model). Reads only the header. */
    fun isMoe(f: File): Boolean = runCatching {
        val cap = minOf(f.length(), PROBE_BYTES.toLong()).toInt()
        val buf = ByteArray(cap)
        f.inputStream().use { s ->
            var off = 0
            while (off < cap) {
                val n = s.read(buf, off, cap - off)
                if (n <= 0) break
                off += n
            }
        }
        indexOf(buf, MOE_MARKER) >= 0
    }.getOrDefault(false)

    /** List MoE models only. Blocking header reads — call off the main thread. */
    fun listMoeModels(ctx: Context): List<File> = allGguf(ctx).filter { isMoe(it) }

    /** Absolute path of libbmoe-cli.so inside the app's nativeLibraryDir. */
    fun cliPath(ctx: Context): String =
        File(ctx.applicationInfo.nativeLibraryDir, "libbmoe-cli.so").absolutePath

    fun pushHint(): String =
        "No MoE .gguf found. Grant all-files access, then push a model with:\n" +
            "adb push model.gguf /sdcard/Download/"

    private fun indexOf(haystack: ByteArray, needle: ByteArray): Int {
        if (needle.isEmpty() || haystack.size < needle.size) return -1
        outer@ for (i in 0..haystack.size - needle.size) {
            for (j in needle.indices) if (haystack[i + j] != needle[j]) continue@outer
            return i
        }
        return -1
    }
}
