package io.bigmoeonedge.app.engine

import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL
import java.security.SecureRandom

/**
 * A non-2xx answer from the server: the message its JSON body carried, for a person, and its
 * [code], for the app to decide on (docs/server-api.md). Empty when the body had none.
 */
class ApiException(val status: Int, message: String, val code: String = "") : IOException(message) {
    companion object {
        /** The code a full context is reported with: the fix is a new chat, and the screen says so. */
        const val CODE_CONTEXT_FULL = "context_full"

        /** The code a turn is refused with when no model is loaded: the fix is on Models. */
        const val CODE_NO_MODEL = "no_model"
    }
}

/**
 * The client of bmoe-server's HTTP API (docs/server-api.md), the one API every UI of this
 * project speaks. Plain HttpURLConnection and org.json: the server is on loopback and the bodies
 * are small, so nothing heavier earns its place. Every call blocks; callers run them off the main
 * thread (the store does, on the IO dispatcher).
 *
 * The key the server was started with is sent as a bearer token on every request: the app always
 * starts it with one, since loopback on a phone is reachable by every installed app.
 *
 * Every call has a read timeout fitted to what it waits for, so a server that stopped answering is
 * an error and not a screen that hangs; only the chat stream and a decision, which wait their turn
 * behind a generation and then a prefill of minutes, wait without one. The event stream has a
 * watchdog instead: the server writes to it at least every [PING_INTERVAL_MS].
 */
class Api(private val baseUrl: String, private val apiKey: String) {

    // ── server and session ──

    fun info(): JSONObject = getJson("/api/info", QUICK_MS)
    fun params(): JSONArray = JSONArray(get("/api/params", QUICK_MS))
    fun config(): JSONObject = getJson("/api/config", QUICK_MS)

    /** Set `values` in the user layer and reset `reset` to the server's defaults. */
    fun updateConfig(values: JSONObject, reset: List<String> = emptyList(), autoPlan: Boolean? = null): JSONObject {
        val body = JSONObject().put("values", values).put("reset", JSONArray(reset))
        if (autoPlan != null) body.put("auto_plan", autoPlan)
        return send("PUT", "/api/config", body, QUICK_MS)
    }

    // A load answers once it is under way, but first unloads the model and, in auto mode, measures
    // the phone; an unload waits out a load in progress. Minutes, not seconds, on a large model.
    fun load(modelPath: String? = null): JSONObject =
        send("POST", "/api/session/load", if (modelPath == null) null else JSONObject().put("model", modelPath), SLOW_MS)

    fun unload(): JSONObject = send("POST", "/api/session/unload", null, SLOW_MS)
    fun cancel(): JSONObject = send("POST", "/api/cancel", null, QUICK_MS)

    /** A decision (docs/decide.md): the model picks one of the choices from a single prefill. */
    fun decide(body: JSONObject): JSONObject = send("POST", "/api/decide", body, NO_TIMEOUT)

    // ── models and the plan ──

    fun models(): JSONObject = getJson("/api/models", QUICK_MS)
    fun plan(measure: Boolean): JSONObject = getJson(if (measure) "/api/plan?measure=1" else "/api/plan", SLOW_MS)
    fun applyPlan(): JSONObject = send("POST", "/api/plan/apply", null, QUICK_MS)

    // ── whose server this is ──

    /**
     * Whether the server on the port is the one this app started: it refuses a key it was not given
     * and accepts the one it was. Another program's server answers the first (an open one) or
     * refuses the second (one with its own key), and a server that accepted a random key it cannot
     * know is not one to hand a conversation to. Throws what [info] throws when nothing answers.
     */
    fun isOurs(): Boolean {
        if (Api(baseUrl, randomKey()).status("/api/info") != 401) return false
        return status("/api/info") == 200
    }

    private fun status(path: String): Int {
        val c = open("GET", path, write = false, readTimeoutMs = QUICK_MS)
        try {
            return c.responseCode
        } finally {
            c.disconnect()
        }
    }

    // ── streams ──

    /**
     * Holds `GET /api/events` open and hands every event to [onEvent] until the server closes the
     * stream, [keepGoing] answers false, or the connection fails (an IOException). Returns normally
     * when the server ended it.
     */
    fun events(onEvent: (SseEvent) -> Unit, keepGoing: () -> Boolean) {
        // The watchdog: the server pings every PING_INTERVAL_MS, so a read that waits two pings
        // and some is a stream that died without closing (a server stopped, not killed; a socket
        // the system dropped). It ends in a SocketTimeoutException, and the caller reconnects.
        stream("GET", "/api/events", null, onEvent, keepGoing, EVENTS_READ_TIMEOUT_MS)
    }

    /**
     * `POST /v1/chat/completions` with `stream: true`: every `data:` payload goes to [onData] as
     * it arrives, `[DONE]` included. A 4xx/5xx is an [ApiException] before anything streams.
     */
    fun chatStream(body: JSONObject, onData: (String) -> Unit, keepGoing: () -> Boolean) {
        stream("POST", "/v1/chat/completions", body, { onData(it.data) }, keepGoing, NO_TIMEOUT)
    }

    // ── plumbing ──

    private fun open(method: String, path: String, write: Boolean, readTimeoutMs: Int): HttpURLConnection {
        val c = URL(baseUrl + path).openConnection() as HttpURLConnection
        c.requestMethod = method
        c.connectTimeout = CONNECT_MS
        c.readTimeout = readTimeoutMs
        c.useCaches = false
        c.setRequestProperty("Accept", "application/json, text/event-stream")
        // Every write says it is JSON, body or no body: the server refuses one that does not.
        if (write) c.setRequestProperty("Content-Type", "application/json")
        c.setRequestProperty("Authorization", "Bearer $apiKey")
        return c
    }

    private fun get(path: String, readTimeoutMs: Int): String {
        val c = open("GET", path, write = false, readTimeoutMs = readTimeoutMs)
        try {
            return readOrThrow(c)
        } finally {
            c.disconnect()
        }
    }

    private fun getJson(path: String, readTimeoutMs: Int): JSONObject = JSONObject(get(path, readTimeoutMs))

    private fun send(method: String, path: String, body: JSONObject?, readTimeoutMs: Int): JSONObject {
        val c = open(method, path, write = true, readTimeoutMs = readTimeoutMs)
        try {
            c.doOutput = true
            val bytes = (body?.toString() ?: "").toByteArray()
            c.setFixedLengthStreamingMode(bytes.size)
            c.outputStream.use { it.write(bytes) }
            val text = readOrThrow(c)
            return if (text.isBlank()) JSONObject() else JSONObject(text)
        } finally {
            c.disconnect()
        }
    }

    private fun readOrThrow(c: HttpURLConnection): String {
        val code = c.responseCode
        if (code in 200..299) return c.inputStream.use { it.readBytes().toString(Charsets.UTF_8) }
        val text = runCatching { c.errorStream?.use { it.readBytes().toString(Charsets.UTF_8) } }.getOrNull().orEmpty()
        throw ApiException(code, errorMessage(code, text), errorCode(text))
    }

    private fun stream(
        method: String,
        path: String,
        body: JSONObject?,
        onEvent: (SseEvent) -> Unit,
        keepGoing: () -> Boolean,
        readTimeoutMs: Int,
    ) {
        val c = open(method, path, write = body != null, readTimeoutMs = readTimeoutMs)
        try {
            if (body != null) {
                c.doOutput = true
                val bytes = body.toString().toByteArray()
                c.setFixedLengthStreamingMode(bytes.size)
                c.outputStream.use { it.write(bytes) }
            }
            val code = c.responseCode
            if (code !in 200..299) {
                val text = runCatching { c.errorStream?.use { it.readBytes().toString(Charsets.UTF_8) } }.getOrNull().orEmpty()
                throw ApiException(code, errorMessage(code, text), errorCode(text))
            }
            val parser = SseParser()
            InputStreamReader(c.inputStream, Charsets.UTF_8).use { reader ->
                val buf = CharArray(8192)
                while (keepGoing()) {
                    val n = reader.read(buf)
                    if (n < 0) {
                        parser.end().forEach(onEvent)
                        return
                    }
                    parser.push(String(buf, 0, n)).forEach(onEvent)
                }
            }
        } finally {
            // Closing the connection is how a client gives its place up: a chat the user stopped,
            // an events stream the app no longer wants.
            c.disconnect()
        }
    }

    companion object {
        private const val CONNECT_MS = 5_000
        /** A read the server answers from memory: the config, the lists, a cancel. */
        private const val QUICK_MS = 15_000
        /** A load, an unload, a plan that measures the phone: tens of seconds, minutes at worst. */
        private const val SLOW_MS = 10 * 60_000
        /** Wait as long as it takes: a chat stream and a decision wait their turn, then prefill. */
        private const val NO_TIMEOUT = 0

        /** How often the server writes a keep-alive to the event stream (docs/server-api.md). */
        const val PING_INTERVAL_MS = 15_000
        /** Two missed pings and a margin: the event stream is dead and is reconnected. */
        const val EVENTS_READ_TIMEOUT_MS = 2 * PING_INTERVAL_MS + 10_000

        /** A key nobody can guess: 32 random bytes, hex. What the app starts the server with. */
        fun randomKey(): String {
            val b = ByteArray(32)
            SecureRandom().nextBytes(b)
            return b.joinToString("") { "%02x".format(it) }
        }

        /** The server's `{"code": "..."}` beside the message, or "" when it sent none. */
        fun errorCode(text: String): String =
            runCatching { JSONObject(text).optString("code") }.getOrNull().orEmpty()

        /** The server's `{"error": "..."}` when it sent one, otherwise the bare status. */
        fun errorMessage(code: Int, text: String): String =
            runCatching { JSONObject(text).optString("error") }.getOrNull()?.takeIf { it.isNotEmpty() }
                ?: text.takeIf { it.isNotBlank() && it.length < 300 }
                ?: "HTTP $code"
    }
}
