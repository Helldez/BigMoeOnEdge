import Foundation

/// One Server-Sent Event: the `event:` name ("message" when absent) and the joined `data:` lines.
public struct SSEEvent: Equatable, Sendable {
    public let event: String
    public let data: String

    public init(event: String, data: String) {
        self.event = event
        self.data = data
    }
}

/// Incremental Server-Sent Events parser (WHATWG event-stream rules). Network chunks can split a
/// line anywhere, so the parser buffers until a line terminator arrives. The same contract as the
/// web UI's and the Android app's parsers, so every client reads the server alike.
public struct SSEParser {
    private var buffer = String.UnicodeScalarView()
    private var dataLines: [String] = []
    private var eventName = ""
    private var hasData = false
    // The last line ended in '\r', which may be the first half of a "\r\n" in the next chunk.
    private var skipLF = false

    public init() {}

    /// Feeds one decoded text chunk; returns the events it completed, in order. Scalars, not
    /// Characters: Swift reads "\r\n" as one Character, and a chunk can end between the two.
    public mutating func push(_ chunk: String) -> [SSEEvent] {
        var out: [SSEEvent] = []
        for c in chunk.unicodeScalars {
            if skipLF {
                skipLF = false
                if c == "\n" { continue }
            }
            if c == "\n" || c == "\r" {
                line(String(buffer), into: &out)
                buffer = String.UnicodeScalarView()
                skipLF = c == "\r"
            } else {
                buffer.append(c)
            }
        }
        return out
    }

    /// The stream ended: a last line without its terminator still counts, and a pending event is
    /// dropped, as the specification has it (only a blank line dispatches one).
    public mutating func end() -> [SSEEvent] {
        var out: [SSEEvent] = []
        if !buffer.isEmpty {
            line(String(buffer), into: &out)
            buffer = String.UnicodeScalarView()
        }
        skipLF = false
        dataLines.removeAll()
        eventName = ""
        hasData = false
        return out
    }

    private mutating func line(_ line: String, into out: inout [SSEEvent]) {
        if line.isEmpty {
            if hasData { out.append(SSEEvent(event: eventName.isEmpty ? "message" : eventName, data: dataLines.joined(separator: "\n"))) }
            dataLines.removeAll()
            eventName = ""
            hasData = false
            return
        }
        if line.hasPrefix(":") { return } // a comment: the server's keep-alive
        let field: Substring
        var value: Substring
        if let colon = line.firstIndex(of: ":") {
            field = line[..<colon]
            value = line[line.index(after: colon)...]
        } else {
            field = line[...]
            value = ""
        }
        if value.hasPrefix(" ") { value = value.dropFirst() }
        switch field {
        case "event": eventName = String(value)
        case "data":
            dataLines.append(String(value))
            hasData = true
        default: break // id and retry are not used here
        }
    }
}
