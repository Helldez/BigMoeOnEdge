package io.bigmoeonedge.app

import io.bigmoeonedge.app.data.FileRole
import io.bigmoeonedge.app.data.RowState
import io.bigmoeonedge.app.data.Tone
import io.bigmoeonedge.app.data.fitTone
import io.bigmoeonedge.app.data.hfStep
import io.bigmoeonedge.app.data.libraryRows
import io.bigmoeonedge.app.engine.ModelsList
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The Models screen's one list: the files the server lists and the catalog's entries merged by
 * file, each row in one state, suggestions last. The downloads are the app's own transfers.
 */
class LibraryRowsTest {
    private fun file(name: String) = """{"name":"$name","bytes":5,"url":"https://example.invalid/$name"}"""

    private val list = ModelsList.of(
        JSONObject(
            """{"models_dir":"/app/models","scan_dirs":["/push"],"local":[
                 {"name":"a.gguf","path":"/app/models/a.gguf","dir":"/app/models","bytes":5,"shards":1,"files":["/app/models/a.gguf"],"role":"model","complete":true},
                 {"name":"s-00001-of-00002.gguf","path":"/app/models/s-00001-of-00002.gguf","dir":"/app/models","bytes":5,"shards":2,
                  "files":["/app/models/s-00001-of-00002.gguf"],"role":"model","complete":false},
                 {"name":"x.gguf","path":"/push/x.gguf","dir":"/push","folder":"","bytes":5,"shards":1,"files":["/push/x.gguf"],"role":"model","complete":true},
                 {"name":"t.gguf","path":"/push/t/t.gguf","dir":"/push","folder":"t","bytes":5,"shards":1,"files":["/push/t/t.gguf"],"role":"model","complete":true},
                 {"name":"mmproj.gguf","path":"/app/models/mmproj.gguf","dir":"/app/models","bytes":1,"shards":1,"files":["/app/models/mmproj.gguf"],"role":"projector","complete":true}
               ],"catalog":[
                 {"id":"a","title":"Model A","file":"a.gguf","bytes":5,"files":[${file("a.gguf")}],"status":"on_disk"},
                 {"id":"s","title":"Split","file":"s-00001-of-00002.gguf","bytes":10,"status":"available","partial":true,"received":7,
                  "files":[${file("s-00001-of-00002.gguf")},${file("s-00002-of-00002.gguf")}]},
                 {"id":"d","title":"Fetching","file":"d.gguf","bytes":5,"files":[${file("d.gguf")}],"status":"available"},
                 {"id":"n","title":"Suggestion","file":"n.gguf","bytes":5,"files":[${file("n.gguf")}],"status":"available"},
                 {"id":"url:u.gguf","title":"u.gguf","file":"u.gguf","bytes":0,"files":[${file("u.gguf")}],"status":"available","added":true}
               ]}""",
        ),
    )
    private val models = FileRole.split(list.local) { it.role }.first

    @Test
    fun aDownloadedEntryIsOneRowWithItsFileAndSuggestionsComeLast() {
        val rows = libraryRows(list, models, setOf("d.gguf"), loadedPath = "/app/models/a.gguf")
        assertEquals(
            listOf("/app/models/a.gguf", "/app/models/s-00001-of-00002.gguf", "/push/x.gguf", "/push/t/t.gguf", "catalog:d", "catalog:url:u.gguf", "catalog:n"),
            rows.map { it.key },
        )
        assertEquals(listOf(RowState.LOADED, RowState.PARTIAL, RowState.ON_DEVICE, RowState.ON_DEVICE, RowState.DOWNLOADING, RowState.AVAILABLE, RowState.AVAILABLE), rows.map { it.state })
        assertEquals("Model A", rows[0].title)
        assertEquals("a", rows[0].entry?.id)
        // An entry added from a link is the person's, never a suggestion; the catalog's untouched one is.
        assertEquals(listOf(false, false, false, false, false, false, true), rows.map { it.suggested })
    }

    @Test
    fun theServerDeletesOnlyInItsOwnDirectoryByTheListedName() {
        val rows = libraryRows(list, models, emptySet(), loadedPath = null)
        assertEquals("a.gguf", rows[0].deleteName)
        assertNull(rows[2].deleteName)
        assertNull(rows.last().deleteName)
    }

    @Test
    fun aSetWithShardsMissingAndNoStoppedDownloadIsIncomplete() {
        val noPart = list.copy(catalog = list.catalog.map { if (it.id == "s") it.copy(partial = false) else it })
        assertEquals(RowState.INCOMPLETE, libraryRows(noPart, models, emptySet(), null)[1].state)
        // Its transfer running outranks both.
        assertEquals(RowState.DOWNLOADING, libraryRows(noPart, models, setOf("s-00002-of-00002.gguf"), null)[1].state)
        val noEntry = list.copy(catalog = emptyList())
        assertEquals(RowState.INCOMPLETE, libraryRows(noEntry, models, emptySet(), null)[1].state)
    }

    @Test
    fun theVerdictIsColouredByTheServersCode() {
        assertEquals(Tone.OK, fitTone("fits"))
        assertEquals(Tone.WARN, fitTone("no_streaming"))
        assertEquals(Tone.ERR, fitTone("no_disk"))
        assertEquals(Tone.NONE, fitTone("unknown"))
        assertEquals(Tone.NONE, fitTone("a code added later"))
        // An older server sends no code: its mode is the next best thing.
        assertEquals(Tone.OK, fitTone("", "streams"))
        assertEquals(Tone.ERR, fitTone("", "not a model"))
    }

    @Test
    fun theStepIsWhatThePersonHasNotWhatTheyLastPressed() {
        assertEquals(0, hfStep(listed = false, chosen = false, checked = false, added = false))
        assertEquals(1, hfStep(listed = true, chosen = false, checked = false, added = false))
        assertEquals(2, hfStep(listed = true, chosen = true, checked = false, added = false))
        assertEquals(3, hfStep(listed = true, chosen = true, checked = true, added = false))
    }
}
