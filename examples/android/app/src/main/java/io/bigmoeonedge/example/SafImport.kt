package io.bigmoeonedge.example

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Imports a gguf the user picked with the system file picker (Storage Access Framework). The
 * picker hands back a content:// URI, but the bmoe-cli subprocess needs a real filesystem path,
 * so we stream the bytes into the app-internal models dir (no permission required, and on a real
 * f2fs/ext4 volume where O_DIRECT works — see ModelManager.internalModelsDir). This is the
 * Play-flavor path for a model already downloaded with the browser — public Downloads is not
 * readable without all-files access.
 *
 * Copied as `<name>.gguf.part`, renamed on completion, so a partial copy is never listed.
 */
object SafImport {
    private const val PART_SUFFIX = ".part"

    /** Copy the picked document into app storage, reporting progress. Runs on the IO dispatcher. */
    suspend fun importGguf(
        ctx: Context,
        uri: Uri,
        onProgress: (copied: Long, total: Long) -> Unit,
    ): Result<File> = withContext(Dispatchers.IO) {
        runCatching {
            val (displayName, size) = queryNameAndSize(ctx, uri)
            val name = displayName.filter { it.isLetterOrDigit() || it in "._-" }
            require(name.endsWith(".gguf")) { "please pick a .gguf file" }

            // Import into app-internal storage (real f2fs/ext4), not the emulated external dir:
            // O_DIRECT works there, so the streamed model reads back at full speed.
            val dir = ModelManager.internalModelsDir(ctx)
            val finalFile = File(dir, name)
            if (finalFile.isFile) return@runCatching finalFile

            if (size > 0 && size > dir.usableSpace) {
                error("not enough free space (need ${size shr 20} MiB)")
            }

            val part = File(dir, name + PART_SUFFIX)
            try {
                val input = ctx.contentResolver.openInputStream(uri) ?: error("cannot open the picked file")
                input.use { src ->
                    part.outputStream().use { dst ->
                        val buf = ByteArray(1 shl 20)
                        var copied = 0L
                        while (true) {
                            val n = src.read(buf)
                            if (n < 0) break
                            dst.write(buf, 0, n)
                            copied += n
                            onProgress(copied, size)
                        }
                        dst.flush()
                    }
                }
                if (!part.renameTo(finalFile)) error("could not finalize the imported file")
                finalFile
            } catch (t: Throwable) {
                part.delete()
                throw t
            }
        }
    }

    private fun queryNameAndSize(ctx: Context, uri: Uri): Pair<String, Long> {
        var name = uri.lastPathSegment?.substringAfterLast('/') ?: "model.gguf"
        var size = -1L
        ctx.contentResolver.query(uri, null, null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                val nameIdx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (nameIdx >= 0 && !c.isNull(nameIdx)) name = c.getString(nameIdx)
                val sizeIdx = c.getColumnIndex(OpenableColumns.SIZE)
                if (sizeIdx >= 0 && !c.isNull(sizeIdx)) size = c.getLong(sizeIdx)
            }
        }
        return name to size
    }
}
