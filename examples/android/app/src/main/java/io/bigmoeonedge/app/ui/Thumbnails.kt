package io.bigmoeonedge.app.ui

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.LruCache
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import io.bigmoeonedge.app.chat.ImageAttachments
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** The thumbnails are small and a transcript scrolls past the same ones: a few dozen stay decoded. */
private val thumbs = LruCache<String, Bitmap>(48)

/**
 * The image a data URL carries, decoded small: the sent bytes are the photo as the camera wrote it
 * (the engine gets it whole), and decoding that at full size for a thumbnail would take the
 * memory of several screens.
 */
private fun decodeThumb(dataUrl: String, maxPx: Int): Bitmap? {
    thumbs.get(dataUrl)?.let { return it }
    val bytes = ImageAttachments.bytesOf(dataUrl) ?: return null
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
    if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
    var sample = 1
    while (bounds.outWidth / (sample * 2) >= maxPx && bounds.outHeight / (sample * 2) >= maxPx) sample *= 2
    val opts = BitmapFactory.Options().apply { inSampleSize = sample }
    return BitmapFactory.decodeByteArray(bytes, 0, bytes.size, opts)?.also { thumbs.put(dataUrl, it) }
}

/** One attached image as a rounded square; a blank tile of the same size while it decodes or when it cannot be. */
@Composable
fun ImageThumb(dataUrl: String, size: Dp, modifier: Modifier = Modifier) {
    val bitmap by produceState<Bitmap?>(initialValue = thumbs.get(dataUrl), dataUrl) {
        if (value == null) value = withContext(Dispatchers.IO) { decodeThumb(dataUrl, 256) }
    }
    Surface(color = tokens.surface2, shape = Corner, modifier = modifier.size(size).clip(Corner)) {
        Box {
            bitmap?.let { Image(it.asImageBitmap(), contentDescription = "Image", contentScale = ContentScale.Crop, modifier = Modifier.size(size)) }
        }
    }
}

/** The thumbnail of a message's images: enough to tell them apart, not enough to read them. */
val MessageThumb = 96.dp
