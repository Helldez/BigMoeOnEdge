package io.bigmoeonedge.app

import io.bigmoeonedge.app.chat.ChatItem
import io.bigmoeonedge.app.chat.ImageAttachments
import io.bigmoeonedge.app.chat.ItemReducer
import io.bigmoeonedge.app.engine.ModelInfo
import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Images with a message (docs/server-api.md, "Images"): the rules the web composer has, the bodies, the shapes. */
class ImageAttachmentsTest {
    private val png = "data:image/png;base64,iVBORw0KGgo="
    private val jpg = "data:image/jpeg;base64,/9j/4AAQ"

    // ── the request bodies ──

    @Test
    fun aMessageCarriesImagesOnlyWhenThereAreAny() {
        val plain = ItemReducer.messageBody("hi")
        assertEquals("hi", plain.getString("text"))
        assertFalse(plain.has("images"))
        assertEquals(1, plain.length())

        val withImages = ItemReducer.messageBody("what is this?", listOf(png, jpg))
        assertEquals("what is this?", withImages.getString("text"))
        val arr = withImages.getJSONArray("images")
        assertEquals(2, arr.length())
        assertEquals(png, arr.getString(0))
        assertEquals(jpg, arr.getString(1))
        assertEquals(2, withImages.length())
    }

    @Test
    fun anEditCarriesTheImagesItIsGivenAndNoneOtherwise() {
        val plain = ItemReducer.editBody("u3", "again")
        assertEquals("u3", plain.getString("item"))
        assertEquals("again", plain.getString("text"))
        assertFalse(plain.has("images"))

        val withImages = ItemReducer.editBody("u3", "again", listOf(png))
        assertEquals("u3", withImages.getString("item"))
        assertEquals(listOf(png), (0 until withImages.getJSONArray("images").length()).map { withImages.getJSONArray("images").getString(it) })
    }

    // ── the shapes ──

    @Test
    fun aUserItemCarriesItsImagesAndOneWithoutReadsAsNone() {
        val item = ChatItem.of(JSONObject("""{"id":"u1","kind":"user","text":"look","images":["$png","$jpg"]}"""))
        assertEquals(listOf(png, jpg), item.images)
        assertEquals("look", item.text)
        assertTrue(ChatItem.of(JSONObject("""{"id":"u2","kind":"user","text":"plain"}""")).images.isEmpty())
        assertTrue(ChatItem.of(JSONObject("""{"id":"u3","kind":"user","text":"","images":null}""")).images.isEmpty())
    }

    @Test
    fun theModelSaysWhetherItReadsImages() {
        assertTrue(ModelInfo.of(JSONObject("""{"name":"m","vision":true}"""))!!.vision)
        assertFalse(ModelInfo.of(JSONObject("""{"name":"m","vision":false}"""))!!.vision)
        assertFalse(ModelInfo.of(JSONObject("""{"name":"m"}"""))!!.vision)
    }

    // ── the rules, worded as the web composer words them ──

    @Test
    fun onlyImagesUpToTheSizeAndCountLimitsAreAccepted() {
        assertNull(ImageAttachments.accept("image/png", 1234, 0))
        assertNull(ImageAttachments.accept("image/jpeg", ImageAttachments.MAX_BYTES, ImageAttachments.MAX_IMAGES - 1))
        assertEquals("Only images can be attached", ImageAttachments.accept("application/pdf", 10, 0))
        assertEquals("Only images can be attached", ImageAttachments.accept(null, 10, 0))
        assertEquals("An image can be at most 16 MB", ImageAttachments.accept("image/png", ImageAttachments.MAX_BYTES + 1, 0))
        assertEquals("At most 8 images go in one message", ImageAttachments.accept("image/png", 10, ImageAttachments.MAX_IMAGES))
        // A file that is not an image is refused for that before anything else.
        assertEquals("Only images can be attached", ImageAttachments.accept("text/plain", ImageAttachments.MAX_BYTES + 1, ImageAttachments.MAX_IMAGES))
        assertEquals(8, ImageAttachments.MAX_IMAGES)
        assertEquals(16L * 1024 * 1024, ImageAttachments.MAX_BYTES)
    }

    @Test
    fun aDataUrlIsTheBytesBase64OnOneLineAndReadsBack() {
        val bytes = ByteArray(300) { it.toByte() }
        val url = ImageAttachments.dataUrl("image/png", bytes)
        assertTrue(url.startsWith("data:image/png;base64,"))
        assertFalse(url.contains('\n'))
        assertArrayEquals(bytes, ImageAttachments.bytesOf(url))
        // No MIME, or one that is not an image, is sent as the default.
        assertTrue(ImageAttachments.dataUrl(null, bytes).startsWith("data:image/jpeg;base64,"))
        assertTrue(ImageAttachments.dataUrl("application/octet-stream", bytes).startsWith("data:image/jpeg;base64,"))
        assertNull(ImageAttachments.bytesOf("https://example.com/a.png"))
        assertNull(ImageAttachments.bytesOf("data:image/png,notbase64"))
    }
}
