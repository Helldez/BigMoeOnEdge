package io.bigmoeonedge.app

import io.bigmoeonedge.app.engine.ListenPort
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.nio.file.Files

/** The port file the server writes (server/listen_port.h) and the port a person types. */
class ListenPortTest {
    @Test
    fun portFileIsDigitsAndANewline() {
        assertEquals(8765, ListenPort.parse("8765\n"))
        assertEquals(65535, ListenPort.parse("65535\n"))
        // Caught half written: no newline yet.
        assertNull(ListenPort.parse("8765"))
        assertNull(ListenPort.parse(""))
        assertNull(ListenPort.parse("\n"))
        assertNull(ListenPort.parse("0\n"))
        assertNull(ListenPort.parse("65536\n"))
        assertNull(ListenPort.parse("123456\n"))
        assertNull(ListenPort.parse(" 8765\n"))
        assertNull(ListenPort.parse("-1\n"))
        assertNull(ListenPort.parse("87a5\n"))
    }

    @Test
    fun portFileReadFromDisk() {
        val dir = Files.createTempDirectory("bmoe-port").toFile()
        val f = java.io.File(dir, "server.port")
        assertNull(ListenPort.read(f))
        f.writeText("9123\n")
        assertEquals(9123, ListenPort.read(f))
        dir.deleteRecursively()
    }

    @Test
    fun aTypedPortIsEmptyForTheDefaultOrOneAnAppMayListenOn() {
        assertEquals(0, ListenPort.chosen(""))
        assertEquals(0, ListenPort.chosen("  "))
        assertEquals(9000, ListenPort.chosen(" 9000 "))
        assertEquals(ListenPort.MIN, ListenPort.chosen(ListenPort.MIN.toString()))
        assertEquals(ListenPort.MAX, ListenPort.chosen(ListenPort.MAX.toString()))
        assertNull(ListenPort.chosen((ListenPort.MIN - 1).toString()))
        assertNull(ListenPort.chosen((ListenPort.MAX + 1).toString()))
        assertNull(ListenPort.chosen("80a"))
    }
}
