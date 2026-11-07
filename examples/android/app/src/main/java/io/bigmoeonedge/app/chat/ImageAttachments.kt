package io.bigmoeonedge.app.chat

import java.util.Base64

/**
 * What may go with a message as an image: the same rules as the web composer, so one message is
 * refused for the same reason on every UI. Pure, so the rules are tested without Android; the
 * screen does the picking and the reading, and asks here before it attaches.
 */
object ImageAttachments {
    /** How many images one message carries at most: the server's own limit (docs/server-api.md, "Images"). */
    const val MAX_IMAGES = 8

    /** The largest image attached as it is: the engine decodes it and the projector downsizes it. */
    const val MAX_BYTES = 16L * 1024 * 1024

    /** The MIME an image is sent as when the resolver names none. */
    const val DEFAULT_MIME = "image/jpeg"

    /**
     * Why a file cannot be attached, worded as the web composer words it; null when it can.
     * [count] is how many images are attached already.
     */
    fun accept(mime: String?, bytes: Long, count: Int): String? = when {
        mime == null || !mime.startsWith("image/") -> "Only images can be attached"
        bytes > MAX_BYTES -> "An image can be at most ${MAX_BYTES / (1024 * 1024)} MB"
        count >= MAX_IMAGES -> "At most $MAX_IMAGES images go in one message"
        else -> null
    }

    /** The image as the API carries it: a data URL, the bytes base64 on one line, not re-encoded. */
    fun dataUrl(mime: String?, bytes: ByteArray): String =
        "data:" + (mime?.takeIf { it.startsWith("image/") } ?: DEFAULT_MIME) + ";base64," + Base64.getEncoder().encodeToString(bytes)

    /** The bytes a data URL carries, for a thumbnail; null when it is not a base64 data URL. */
    fun bytesOf(dataUrl: String): ByteArray? {
        if (!dataUrl.startsWith("data:")) return null
        val comma = dataUrl.indexOf(',')
        if (comma < 0 || !dataUrl.substring(0, comma).endsWith(";base64")) return null
        return runCatching { Base64.getDecoder().decode(dataUrl.substring(comma + 1)) }.getOrNull()
    }
}
