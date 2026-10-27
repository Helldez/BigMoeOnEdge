package io.bigmoeonedge.app.agent

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import android.webkit.MimeTypeMap
import androidx.core.content.FileProvider
import java.io.File

/**
 * The phone's workspaces: folders inside the app's own storage. The server needs a real folder
 * path, and on a phone the one place this app may read and write freely without a storage
 * permission is its files directory; files reach a folder through the system picker (copied in)
 * and leave it through the share sheet.
 */
object LocalFolders {
    fun root(ctx: Context): File = File(ctx.filesDir, "workspaces").apply { mkdirs() }

    /** A folder name from what the person typed: letters, digits, spaces, dot, dash, underscore. */
    fun safeName(name: String): String =
        name.trim().filter { it.isLetterOrDigit() || it in " ._-" }.trim('.', ' ').take(64)

    /** Makes the folder (a free name beside a taken one) and answers it. */
    fun create(ctx: Context, name: String): File {
        val base = safeName(name).ifEmpty { "Workspace" }
        val root = root(ctx)
        var dir = File(root, base)
        var n = 2
        while (dir.exists()) dir = File(root, "$base ${n++}")
        check(dir.mkdirs()) { "could not create the folder" }
        return dir
    }

    /** Whether [path] is one of this app's folders, which the app can list, import into and share from. */
    fun isLocal(ctx: Context, path: String): Boolean =
        runCatching { File(path).canonicalPath.startsWith(root(ctx).canonicalPath + File.separator) }.getOrDefault(false)

    /** A folder's entries, folders first. */
    fun list(dir: File): List<File> =
        dir.listFiles().orEmpty().filter { !it.name.startsWith(".") }.sortedWith(compareBy({ !it.isDirectory }, { it.name.lowercase() }))

    /** Copies a picked document into [dir], under its own name (a free one beside a taken one). */
    fun importInto(ctx: Context, dir: File, uri: Uri): File {
        var name = uri.lastPathSegment?.substringAfterLast('/') ?: "file"
        ctx.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst() && !c.isNull(0)) name = c.getString(0)
        }
        name = name.replace('/', '_').ifEmpty { "file" }
        var target = File(dir, name)
        var n = 2
        while (target.exists()) {
            val dot = name.lastIndexOf('.')
            target = File(dir, if (dot > 0) name.substring(0, dot) + " ($n)" + name.substring(dot) else "$name ($n)")
            n++
        }
        val input = ctx.contentResolver.openInputStream(uri) ?: error("cannot open the picked file")
        input.use { src -> target.outputStream().use { src.copyTo(it) } }
        return target
    }

    /** The share sheet for one file of a folder, as a content URI with a one-shot read grant. */
    fun shareIntent(ctx: Context, file: File): Intent {
        val uri = FileProvider.getUriForFile(ctx, ctx.packageName + ".fileprovider", file)
        val type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(file.extension.lowercase()) ?: "application/octet-stream"
        val send = Intent(Intent.ACTION_SEND).setType(type).putExtra(Intent.EXTRA_STREAM, uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        return Intent.createChooser(send, file.name)
    }
}
