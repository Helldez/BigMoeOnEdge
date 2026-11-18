import XCTest
@testable import BmoeKit

/// The first run's check of a model: the server's sentence and tone, read and never re-decided.
final class ModelCheckTests: XCTestCase {
    private func check(_ verdict: String) throws -> ModelCheck {
        ModelCheck(try JSON(parsing: #"{"id":"m","bytes":1,"model":{"readable":true},"verdict":"# + verdict + "}"))
    }

    func testTheServersSentenceAndToneAreRead() throws {
        let c = try check(#"{"downloadable":true,"mode":"streams","disk":"d","memory":"m","fit":"fits","headline":"Fits here","summary":"Runs here.","tone":"ok"}"#)
        XCTAssertEqual(c.sentence, "Runs here.")
        XCTAssertEqual(CheckState.done(c).tone, .ok)
        // An older server has no plain sentence: its headline stands in, and no tone is claimed.
        let old = try check(#"{"downloadable":true,"mode":"streams","disk":"d","memory":"m","headline":"Fits here"}"#)
        XCTAssertEqual(old.sentence, "Fits here")
        XCTAssertEqual(CheckState.done(old).tone, FitTone.none)
        XCTAssertEqual(FitTone(tone: "a tone this build does not know"), FitTone.none)
        XCTAssertEqual(CheckState.pending.tone, FitTone.none)
    }

    func testTheDownloadIsHeldWhileCheckingAndWhereRefused() throws {
        let ok = try check(#"{"downloadable":true,"mode":"streams","disk":"","memory":""}"#)
        let no = try check(#"{"downloadable":false,"mode":"streams","disk":"","memory":""}"#)
        XCTAssertFalse(Optional<CheckState>.none.mayDownload)
        XCTAssertFalse(Optional(CheckState.pending).mayDownload)
        XCTAssertTrue(Optional(CheckState.done(ok)).mayDownload)
        XCTAssertFalse(Optional(CheckState.done(no)).mayDownload)
        // A check that could not be made refuses nothing.
        XCTAssertTrue(Optional(CheckState.failed("offline")).mayDownload)
    }
}
