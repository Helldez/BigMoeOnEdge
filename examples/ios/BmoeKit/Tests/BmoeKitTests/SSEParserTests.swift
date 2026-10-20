import XCTest
@testable import BmoeKit

/// The event-stream rules the server's `/api/events` and chat stream rely on.
final class SSEParserTests: XCTestCase {
    func testNamedEventsWithOneDataLine() {
        var p = SSEParser()
        let out = p.push("event: state\ndata: {\"a\":1}\n\nevent: token\ndata: {\"b\":2}\n\n")
        XCTAssertEqual(out, [SSEEvent(event: "state", data: "{\"a\":1}"), SSEEvent(event: "token", data: "{\"b\":2}")])
    }

    func testAChunkMaySplitALineAnywhere() {
        var p = SSEParser()
        let out = p.push("data: hel") + p.push("lo\n") + p.push("\n")
        XCTAssertEqual(out, [SSEEvent(event: "message", data: "hello")])
    }

    func testCRLFAcrossChunksIsOneTerminator() {
        var p = SSEParser()
        let out = p.push("data: x\r") + p.push("\n\r\n")
        XCTAssertEqual(out, [SSEEvent(event: "message", data: "x")])
    }

    func testCRLFInsideOneChunk() {
        var p = SSEParser()
        XCTAssertEqual(p.push("data: y\r\n\r\n"), [SSEEvent(event: "message", data: "y")])
    }

    func testCommentsAreKeepAlivesAndDataLinesJoin() {
        var p = SSEParser()
        XCTAssertEqual(p.push(": ping\n\ndata: a\ndata: b\n\n"), [SSEEvent(event: "message", data: "a\nb")])
    }

    func testAPendingEventWithoutABlankLineIsDroppedAtTheEnd() {
        var p = SSEParser()
        XCTAssertEqual(p.push("data: tail"), [])
        XCTAssertEqual(p.end(), [])
    }

    func testMultibyteTextSurvives() {
        var p = SSEParser()
        XCTAssertEqual(p.push("data: città 🙂\n\n"), [SSEEvent(event: "message", data: "città 🙂")])
    }
}
