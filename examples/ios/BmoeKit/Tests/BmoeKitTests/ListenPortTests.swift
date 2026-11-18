import Foundation
import XCTest
@testable import BmoeKit

/// The port file the server writes (server/listen_port.h) and the port a person types.
final class ListenPortTests: XCTestCase {
    func testPortFileIsDigitsAndANewline() {
        XCTAssertEqual(ListenPort.parse("8765\n"), 8765)
        XCTAssertEqual(ListenPort.parse("65535\n"), 65535)
        XCTAssertNil(ListenPort.parse("8765"), "caught half written: no newline yet")
        for bad in ["", "\n", "0\n", "65536\n", "123456\n", " 8765\n", "-1\n", "87a5\n"] {
            XCTAssertNil(ListenPort.parse(bad), bad)
        }
    }

    func testPortFileReadFromDisk() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let f = dir.appendingPathComponent("server.port")
        XCTAssertNil(ListenPort.read(f))
        try "9123\n".write(to: f, atomically: true, encoding: .utf8)
        XCTAssertEqual(ListenPort.read(f), 9123)
    }

    func testATypedPortIsEmptyForTheDefaultOrOneAnAppMayListenOn() {
        XCTAssertEqual(ListenPort.chosen(""), 0)
        XCTAssertEqual(ListenPort.chosen("  "), 0)
        XCTAssertEqual(ListenPort.chosen(" 9000 "), 9000)
        XCTAssertEqual(ListenPort.chosen(String(ListenPort.min)), ListenPort.min)
        XCTAssertEqual(ListenPort.chosen(String(ListenPort.max)), ListenPort.max)
        XCTAssertNil(ListenPort.chosen(String(ListenPort.min - 1)))
        XCTAssertNil(ListenPort.chosen(String(ListenPort.max + 1)))
        XCTAssertNil(ListenPort.chosen("80a"))
    }
}
