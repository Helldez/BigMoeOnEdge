package io.bigmoeonedge.app.engine

/** One Server-Sent Event: the `event:` name ("message" when absent) and the joined `data:` lines. */
data class SseEvent(val event: String, val data: String)

/**
 * Incremental Server-Sent Events parser (WHATWG event-stream rules). Network chunks can split a
 * line anywhere, so the parser buffers until a line terminator arrives. The same contract as the
 * web UI's parser, so the two clients read the server alike; pure, and tested on its own.
 */
class SseParser {
    private val buffer = StringBuilder()
    private val dataLines = mutableListOf<String>()
    private var eventName = ""
    private var hasData = false
    private var pendingCr = false

    /** Feeds one decoded text chunk; returns the events it completed, in order. */
    fun push(chunk: String): List<SseEvent> {
        val out = mutableListOf<SseEvent>()
        var text = chunk
        // A '\r' at the end of the previous chunk may be the first half of a "\r\n".
        if (pendingCr) {
            pendingCr = false
            if (text.startsWith("\n")) text = text.substring(1)
        }
        buffer.append(text)
        var start = 0
        var i = 0
        while (i < buffer.length) {
            val c = buffer[i]
            if (c != '\n' && c != '\r') {
                i++
                continue
            }
            line(buffer.substring(start, i), out)
            if (c == '\r') {
                if (i + 1 < buffer.length) {
                    if (buffer[i + 1] == '\n') i++
                } else {
                    pendingCr = true
                }
            }
            i++
            start = i
        }
        buffer.delete(0, start)
        return out
    }

    /** The stream ended: a last line without its terminator still counts. */
    fun end(): List<SseEvent> {
        val out = mutableListOf<SseEvent>()
        if (buffer.isNotEmpty()) {
            line(buffer.toString(), out)
            buffer.setLength(0)
        }
        // A pending event is only dispatched by a blank line; without one it is dropped, as the
        // specification has it.
        dataLines.clear()
        eventName = ""
        hasData = false
        return out
    }

    private fun line(line: String, out: MutableList<SseEvent>) {
        if (line.isEmpty()) {
            if (hasData) out += SseEvent(eventName.ifEmpty { "message" }, dataLines.joinToString("\n"))
            dataLines.clear()
            eventName = ""
            hasData = false
            return
        }
        if (line.startsWith(":")) return // a comment: the server's keep-alive
        val colon = line.indexOf(':')
        val field = if (colon < 0) line else line.substring(0, colon)
        var value = if (colon < 0) "" else line.substring(colon + 1)
        if (value.startsWith(" ")) value = value.substring(1)
        when (field) {
            "event" -> eventName = value
            "data" -> {
                dataLines += value
                hasData = true
            }
            else -> {} // id and retry are not used here
        }
    }
}
