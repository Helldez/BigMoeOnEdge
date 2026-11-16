package io.bigmoeonedge.app.engine

import android.content.Context
import java.io.File

/**
 * Where the app's server listens (server/listen_port.h). The server takes the port the person chose,
 * or its own default, and when that one is in use (a remote-access app, another server) the next free
 * one; then it writes the port it took to a file the app names. The app never assumes a port: it
 * speaks, key and all, only to the port its own server wrote, so a port another program holds is
 * never sent the key and never shown as the engine's address.
 */
object ListenPort {
    /** The ports a person may choose. Below 1024 an app may not listen; 0 leaves the server's default. */
    const val MIN = 1024
    const val MAX = 65535

    /** The port file: the app's private storage, kept out of backups like the key beside it. */
    fun file(ctx: Context): File = File(ctx.noBackupFilesDir, "server.port")

    /**
     * The port in a port file's text: digits and a newline, as the server writes it. Null for anything
     * else, a file caught half written included (no newline yet).
     */
    fun parse(text: String): Int? {
        if (!text.endsWith("\n")) return null
        val digits = text.dropLast(1)
        if (digits.isEmpty() || digits.length > 5 || !digits.all { it in '0'..'9' }) return null
        return digits.toInt().takeIf { it in 1..MAX }
    }

    /** The port in [f]; null when there is no such file yet or it does not hold one. */
    fun read(f: File): Int? = runCatching { parse(f.readText()) }.getOrNull()

    /**
     * A port as a person typed it: blank is the server's default (0), a number must be one an app may
     * listen on. Null when it is neither, so the field says so instead of saving it.
     */
    fun chosen(text: String): Int? {
        val t = text.trim()
        if (t.isEmpty()) return 0
        return t.toIntOrNull()?.takeIf { it in MIN..MAX }
    }
}
