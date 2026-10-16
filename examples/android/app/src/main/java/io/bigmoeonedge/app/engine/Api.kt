package io.bigmoeonedge.app.engine

import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL

/**
 * A non-2xx answer from the server: the message its JSON body carried, for a person, and its
 * [code], for the app to decide on (docs/server-api.md). Empty when the body had none.
 */
class ApiException(val status: Int, message: String, val code: String = "") : IOException(message) {
    companion object {
        /** The code a full context is reported with: the fix is a new chat, and the screen says so. */
        const val CODE_CONTEXT_FULL = "context_full"
    }
}

/**
 * The client of bmoe-server's HTTP API (docs/server-api.md), the one API every UI of this
 * project speaks. Plain HttpURLConnection and org.json: the server is on loopback and the bodies
 * are small, so nothing heavier earns its place. Every call blocks; callers run them off the main
 * thread (the store does, on the IO dispatcher).
 *
 * The key, when the server was started with one, is sent as a bearer token on every request.
 */
class Api(private val baseUrl: String, private val apiKey: String? = null) {

    // ── server and session ──

    fun info(): JSONObject = getJson("/api/info")
    fun params(): JSONArray = JSONArray(get("/api/params"))
    fun config(): JSONObject = getJson("/api/config")

    /** Set `values` in the user layer and reset `reset` to the server's defaults. */
    fun updateConfig(values: JSONObject, reset: List<String> = emptyList(), autoPlan: Boolean? = null): JSONObject {
        val body = JSONObject().put("values", values).put("reset", JSONArray(reset))
        if (autoPlan != null) body.put("auto_plan", autoPlan)
        return send("PUT", "/api/config", body)
    }

    fun load(modelPath: String? = null): JSONObject =
        send("POST", "/api/session/load", if (modelPath == null) null else JSONObject().put("model", modelPath))

    fun unload(): JSONObject = send("POST", "/api/session/unload", null)
    fun cancel(): JSONObject = send("POST", "/api/cancel", null)

    /** A decision (docs/decide.md): the model picks one of the choices from a single prefill. */
    fun decide(body: JSONObject): JSONObject = send("POST", "/api/decide", body)

    // ── models and the plan ──

    fun models(): JSONObject = getJson("/api/models")
    fun plan(measure: Boolean): JSONObject = getJson(if (measure) "/api/plan?measure=1" else "/api/plan")
    fun applyPlan(): JSONObject = send("POST", "/api/plan/apply", null)

    // ── streams ──

    /**
     * Holds `GET /api/events` open and hands every event to [onEvent] until the server closes the
     * stream, [keepGoing] answers false, or the connection fails (an IOException). Returns normally
     * when the server ended it.
     */
    fun events(onEvent: (SseEvent) -> Unit, keepGoing: () -> Boolean) {
        stream("GET", "/api/events", null, onEvent, keepGoing)
    }

    /**
     * `POST /v1/chat/completions` with `stream: true`: every `data:` payload goes to [onData] as
     * it arrives, `[DONE]` included. A 4xx/5xx is an [ApiException] before anything streams.
     */
    fun chatStream(body: JSONObject, onData: (String) -> Unit, keepGoing: () -> Boolean) {
        stream("POST", "/v1/chat/completions", body, { onData(it.data) }, keepGoing)
    }

    // ── plumbing ──

    private fun open(method: String, path: String, write: Boolean): HttpURLConnection {
        val c = URL(baseUrl + path).openConnection() as HttpURLConnection
        c.requestMethod = method
        c.connectTimeout = 5_000
        c.readTimeout = 0 // a load, a plan or a stream may take minutes; the caller decides
        c.useCaches = false
        c.setRequestProperty("Accept", "application/json, text/event-stream")
        // Every write says it is JSON, body or no body: the server refuses one that does not.
        if (write) c.setRequestProperty("Content-Type", "application/json")
        if (!apiKey.isNullOrEmpty()) c.setRequestProperty("Authorization", "Bearer $apiKey")
        return c
    }

    private fun get(path: String): String {
        val c = open("GET", path, write = false)
        try {
            return readOrThrow(c)
        } finally {
            c.disconnect()
        }
    }

    private fun getJson(path: String): JSONObject = JSONObject(get(path))

    private fun send(method: String, path: String, body: JSONObject?): JSONObject {
        val c = open(method, path, write = true)
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
    ) {
        val c = open(method, path, write = body != null)
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
