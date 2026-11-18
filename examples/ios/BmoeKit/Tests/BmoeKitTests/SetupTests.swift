import XCTest
@testable import BmoeKit

/// The first run's pure parts: the time left (the web's `eta.test.ts`), the pair of models offered,
/// the step from the state, the facts line.
final class SetupTests: XCTestCase {
    func testTheRateOverTheWindowAndTheTimeLeft() {
        var s = ETA.add([], .init(at: 0, received: 0))
        s = ETA.add(s, .init(at: 5, received: 50_000_000))
        XCTAssertEqual(ETA.rate(s), 10_000_000)
        XCTAssertEqual(ETA.secondsLeft(s, total: 150_000_000), 10)
    }

    func testNothingFromOneSampleOrSamplesUnderASecondApart() {
        XCTAssertNil(ETA.rate([.init(at: 0, received: 0)]))
        XCTAssertNil(ETA.rate([.init(at: 0, received: 0), .init(at: 0.5, received: 10)]))
    }

    func testOldSamplesAndThoseFromBeforeARestartAreDropped() {
        var s = ETA.add([], .init(at: 0, received: 0))
        s = ETA.add(s, .init(at: 30, received: 100))
        XCTAssertEqual(s.count, 1)
        s = ETA.add([.init(at: 0, received: 500)], .init(at: 1, received: 10))
        XCTAssertEqual(s.count, 1)
    }

    func testItRoundsLikeAPerson() {
        XCTAssertEqual(ETA.human(20), "less than a minute")
        XCTAssertEqual(ETA.human(12 * 60 + 10), "about 12 min")
        XCTAssertEqual(ETA.human(80 * 60), "about 1 h 20 min")
        XCTAssertEqual(ETA.human(120 * 60), "about 2 h")
    }

    private func entry(_ id: String, _ gb: Int64, added: Bool = false) -> CatalogEntry {
        CatalogEntry(["id": .string(id), "title": .string(id), "bytes": .int(gb * 1_000_000_000), "added": .bool(added),
                      "files": [["name": .string("\(id).gguf"), "bytes": .int(gb * 1_000_000_000), "url": "https://example.com/m.gguf"]]])!
    }

    func testThePairIsTheLargestThatFitsAndTheSmallestThatDoesNot() {
        let catalog = [entry("a", 4), entry("b", 9), entry("c", 20), entry("d", 30), entry("x", 10, added: true)]
        let ram: Int64 = 12_000_000_000
        XCTAssertEqual(Setup.pair(catalog, ramBytes: ram).map(\.id), ["b", "c"])
        XCTAssertEqual(Setup.recommended(catalog, ramBytes: ram)?.id, "b")
        // Nothing fits: the smallest is suggested, and there is no pair.
        XCTAssertEqual(Setup.recommended(catalog, ramBytes: 1), catalogMin(catalog))
        XCTAssertEqual(Setup.pair(catalog, ramBytes: 1), [])
        XCTAssertEqual(Setup.pair(catalog, ramBytes: 0), [])
        XCTAssertEqual(Setup.kind(catalog[1], ramBytes: ram), "Fits in memory")
        XCTAssertEqual(Setup.kind(catalog[2], ramBytes: ram), "Larger than memory")
    }

    private func catalogMin(_ c: [CatalogEntry]) -> CatalogEntry? { c.first { $0.id == "a" } }

    func testTheStepFollowsTheState() {
        XCTAssertEqual(Setup.step(nil, loading: false), 1)
        XCTAssertEqual(Setup.step(.available, loading: false), 1)
        // A paused download stays the download step, unless the person picks again.
        XCTAssertEqual(Setup.step(.partial, loading: false), 2)
        XCTAssertEqual(Setup.step(.partial, loading: false, repick: true), 1)
        XCTAssertEqual(Setup.step(.downloading, loading: false, repick: true), 2)
        XCTAssertEqual(Setup.step(.downloading, loading: false), 2)
        XCTAssertEqual(Setup.step(.onDevice, loading: false), 3)
        XCTAssertEqual(Setup.step(.available, loading: true), 3)
    }

    func testTheFactsLineSaysOnlyWhatIsKnown() {
        XCTAssertEqual(Setup.facts(ramMiB: 12 * 1024, threads: 8, gpus: ["Apple GPU"]),
                       "12 GiB of memory · 8 processor threads · Apple GPU")
        XCTAssertEqual(Setup.facts(ramMiB: 0, threads: 6, gpus: []), "6 processor threads")
    }

    func testTheWelcomeLineIsTheStateUntilAModelIsReady() {
        typealias I = StatusWords.Input
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .empty), greet: true).text, "No model loaded")
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .loading), greet: true).text, "Loading the model…")
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .ready), greet: true).text,
                       "Private: everything stays on this phone.")
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .ready)).text, "Ready")
    }
}
