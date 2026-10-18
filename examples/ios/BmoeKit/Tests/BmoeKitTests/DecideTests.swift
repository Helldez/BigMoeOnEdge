import XCTest
@testable import BmoeKit

final class DecideTests: XCTestCase {
    func testOptionsAreLetteredAndOnePerLine() {
        let options = Decide.parseOptions("  red \n\nblue\n green ")
        XCTAssertEqual(options, ["red", "blue", "green"])
        XCTAssertEqual(Decide.labels(3), ["A", "B", "C"])
        XCTAssertEqual(Decide.userText("Which?", options), "Which?\nA) red\nB) blue\nC) green")
        let body = Decide.requestBody("Which?", options)
        XCTAssertEqual(body["prefix"].string, "Which?")
        XCTAssertEqual(body["choices"].strings, ["A", "B", "C"])
        XCTAssertTrue(body["suffix"].string(or: "").hasSuffix("Answer with the letter of one option only."))
    }

    func testTheAnswerIsReadAsProbabilities() throws {
        let o = try JSON(parsing: #"{"best":1,"choice_logp":[-2.302585,-0.105360,null],"prefill_s":0.9,"n_prefilled":32,"n_reused":180,"prefix_state_mib":48.0}"#)
        let r = Decide.parse(o, ["a", "b", "c"])
        XCTAssertEqual(r.best, 1)
        XCTAssertEqual(r.scores[0].probability, 0.1, accuracy: 1e-4)
        XCTAssertEqual(r.scores[1].probability, 0.9, accuracy: 1e-4)
        XCTAssertEqual(r.scores[2].probability, 0)
        XCTAssertEqual(r.metrics, "prefill 0.9s (32 tok, 180 reused) · context kept 48 MiB")
    }
}
