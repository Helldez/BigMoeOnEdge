package io.bigmoeonedge.app

import io.bigmoeonedge.app.engine.Api
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.InetAddress
import java.net.ServerSocket
import kotlin.concurrent.thread

/**
 * Whose server is on the port: the one this app started answers its key and refuses any other;
 * an open server, or one with a key of its own, is not handed a conversation. Against a real HTTP
 * server on loopback, standing in for bmoe-server's access rule (docs/server-api.md).
 */
class ApiOwnershipTest {
    /** A minimal HTTP server that requires [key] (none: open), and the Authorization headers it saw. */
    private fun withServer(key: String?, block: (base: String, seen: MutableList<String?>) -> Unit) {
        val seen = mutableListOf<String?>()
        val socket = ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"))
        val loop = thread(isDaemon = true) {
            while (!socket.isClosed) {
                val c = runCatching { socket.accept() }.getOrNull() ?: break
                c.use { conn ->
                    val reader = conn.getInputStream().bufferedReader()
                    var auth: String? = null
                    while (true) {
                        val line = reader.readLine() ?: break
                        if (line.isEmpty()) break
                        if (line.startsWith("Authorization:", ignoreCase = true)) auth = line.substringAfter(':').trim()
                    }
                    synchronized(seen) { seen += auth }
                    val ok = key == null || auth == "Bearer $key"
                    val body = if (ok) """{"version":"t","state":"empty"}""" else """{"error":"an API key is required"}"""
                    val status = if (ok) "200 OK" else "401 Unauthorized"
                    val out = "HTTP/1.1 $status\r\nContent-Type: application/json\r\nContent-Length: ${body.length}\r\n" +
                        "Connection: close\r\n\r\n$body"
                    conn.getOutputStream().apply { write(out.toByteArray()); flush() }
                }
            }
        }
        try {
            block("http://127.0.0.1:${socket.localPort}", seen)
        } finally {
            socket.close()
            loop.join(2000)
        }
    }

    @Test
    fun theServerStartedWithTheKeyIsOurs() = withServer("k1") { base, seen ->
        assertTrue(Api(base, "k1").isOurs())
        // The decoy was a random key, never this one, and never no key at all.
        assertEquals(2, seen.size)
        assertNotEquals("Bearer k1", seen[0])
        assertTrue(seen[0]!!.startsWith("Bearer "))
        assertEquals("Bearer k1", seen[1])
    }

    @Test
    fun anOpenServerIsNotOurs() = withServer(null) { base, _ ->
        assertFalse(Api(base, "k1").isOurs())
    }

    @Test
    fun aServerWithAnotherKeyIsNotOurs() = withServer("someone-else") { base, _ ->
        assertFalse(Api(base, "k1").isOurs())
    }

    @Test
    fun everyCallCarriesTheKey() = withServer("k1") { base, seen ->
        Api(base, "k1").info()
        assertEquals(listOf<String?>("Bearer k1"), seen)
    }

    @Test
    fun aLaunchKeyIsLongAndNeverRepeats() {
        val a = Api.randomKey()
        assertEquals(64, a.length)
        assertNotEquals(a, Api.randomKey())
    }

    @Test
    fun theWatchdogWaitsTwoPingsAndAMargin() {
        assertTrue(Api.EVENTS_READ_TIMEOUT_MS > 2 * Api.PING_INTERVAL_MS)
        assertTrue(Api.EVENTS_READ_TIMEOUT_MS < 3 * Api.PING_INTERVAL_MS)
    }
}
