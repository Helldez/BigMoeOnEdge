import XCTest
@testable import BmoeKit

/// The line under an answer, its figures, and the status words: the web UI's, word for word.
final class TurnFiguresTests: XCTestCase {
    private func summary(_ o: JSON) -> TurnSummary { TurnSummary(o) }

    func testTheLineIsTgThenPpAndPpIsLeftOutWhenUnknown() {
        XCTAssertEqual(TurnFigures.line(summary(["tok_s": 12.43, "prefill_tps": 139.6])), "tg 12.4 tok/s · pp 140 tok/s")
        // No rate from the server: the tokens over the seconds.
        XCTAssertEqual(TurnFigures.line(summary(["tok_s": 9.0, "n_prompt": 126, "prefill_s": 0.9])), "tg 9.0 tok/s · pp 140 tok/s")
        XCTAssertEqual(TurnFigures.line(summary(["tok_s": 9.0])), "tg 9.0 tok/s")
        XCTAssertEqual(TurnFigures.liveLine(rate: 11.06), "tg 11.1 tok/s")
    }

    /// The web's fixture (`ui/src/lib/turnMetrics.test.ts`).
    private let web: JSON = ["cancelled": false, "tokens": 42, "tok_s": 10, "prefill_s": 1.25, "prefill_tps": 13.6, "n_prompt": 17,
                             "n_past": 900, "cache_hit_pct": 81.4, "compute_s_tok": 0.07, "flash_wait_s_tok": 0.015,
                             "mgmt_s_tok": 0.002, "ttft_s": 1.25]

    private func with(_ base: JSON, _ changes: [String: JSON]) -> JSON {
        var o = base.object ?? [:]
        for (k, v) in changes { o[k] = v }
        return .object(o)
    }

    func testATokensTimeSplitsIntoWholePercentagesThatAddUpTo100() {
        let t = TurnFigures.timeShares(summary(web))
        XCTAssertEqual(t.map(\.key), [.compute, .disk, .mgmt, .other])
        XCTAssertEqual(t.reduce(0) { $0 + $1.pct }, 100)
        XCTAssertEqual(t.first { $0.key == .compute }?.pct, 70)
        // Every part rounds down: the remainders still bring it to 100.
        let odd = with(web, ["tok_s": 3, "compute_s_tok": 0.111, "flash_wait_s_tok": 0.111, "mgmt_s_tok": 0.111])
        XCTAssertEqual(TurnFigures.timeShares(summary(odd)).reduce(0) { $0 + $1.pct }, 100)
        XCTAssertEqual(TurnFigures.timeShares(summary(with(web, ["tok_s": 0]))), [])
        XCTAssertEqual(TurnFigures.wholeShares([1, 1, 1]), [34, 33, 33])
    }

    func testTheTimeSplitIsTheOnlyPercentageAndMemoryIsACount() {
        let f = TurnFigures.figures(summary(web), nCtx: 16384, expectedTokS: 13.9)
        XCTAssertEqual(f.map(\.id), [.speed, .time, .cache, .prompt, .context])
        XCTAssertEqual(f[0].value, "10.0 tok/s, plan expected 13.9")
        XCTAssertEqual(f.first { $0.id == .time }?.value,
                       "computing 70% · waiting for the disk 15% · managing the cache 2% · other 13%")
        XCTAssertEqual(f.first { $0.id == .time }?.label, "Where each token's time went")
        XCTAssertEqual(f.first { $0.id == .cache }?.value, "81 of every 100 parts")
        XCTAssertEqual(f.first { $0.id == .context }?.value, "900 of 16,384 tokens")
        XCTAssertEqual(f.first { $0.id == .prompt }?.value, "13.6 tok/s, 17 tokens in 1.3 s")
        XCTAssertEqual(f.filter { $0.id != .time && $0.value.contains("%") }, [])
        for x in f { XCTAssertFalse(x.hint.isEmpty) }
        // Longer than 24 characters, a value goes under its label; short ones stay beside it.
        XCTAssertTrue(f.first { $0.id == .time }!.valueOnOwnLine)
        XCTAssertTrue(f.first { $0.id == .prompt }!.valueOnOwnLine)
        XCTAssertFalse(f.first { $0.id == .cache }!.valueOnOwnLine)
        XCTAssertFalse(TurnFigure.valueOnOwnLine(String(repeating: "x", count: 24)))
        XCTAssertTrue(TurnFigure.valueOnOwnLine(String(repeating: "x", count: 25)))
    }

    /// A model held whole in memory (the web's resident fixture): both speeds, all of its time computing.
    func testAResidentModelStillHasItsSpeeds() {
        let s = summary(with(web, ["tok_s": 93.9, "prefill_s": 0.17, "prefill_tps": 111.6, "n_prompt": 19, "n_past": 25,
                                   "cache_hit_pct": -1, "read_mib": 0, "compute_s_tok": .double(1 / 93.9),
                                   "flash_wait_s_tok": 0, "mgmt_s_tok": 0]))
        XCTAssertEqual(TurnFigures.line(s), "tg 93.9 tok/s · pp 112 tok/s")
        let f = TurnFigures.figures(s, nCtx: 16384, expectedTokS: nil)
        XCTAssertEqual(f.map(\.id), [.speed, .time, .prompt, .context])
        XCTAssertEqual(f.first { $0.id == .prompt }?.value, "111.6 tok/s, 19 tokens in 0.2 s")
        XCTAssertEqual(f.first { $0.id == .time }?.value, "computing 100%")
    }

    func testWhatATurnDidNotMeasureIsNotShown() {
        let f = TurnFigures.figures(summary(with(web, ["cache_hit_pct": -1])), nCtx: 0, expectedTokS: nil)
        XCTAssertEqual(f[0].value, "10.0 tok/s")
        XCTAssertEqual(f.map(\.id), [.speed, .time, .prompt])
        XCTAssertEqual(TurnFigures.figures(summary(with(web, ["cancelled": true])), nCtx: 0, expectedTokS: nil).last?.id, .stopped)
        let drafts = TurnFigures.figures(summary(with(web, ["mtp_decodes": 10, "mtp_drafted": 40, "mtp_accepted": 31])),
                                         nCtx: 0, expectedTokS: nil)
        XCTAssertEqual(drafts.first { $0.id == .drafts }?.value, "31 of 40 guesses")
    }

    func testTheStatusWords() {
        typealias I = StatusWords.Input
        XCTAssertEqual(StatusWords.say(I(reachable: false, engine: nil)).text, "The engine is not running.")
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: nil)).text, "Connecting…")
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .planning)).text, "Measuring this phone…")
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .loading)).text, "Loading the model…")
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .error)).text, "The model did not load. See Models.")
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .empty)).text, "No model loaded")
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .ready)).text, "Ready")
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .ready, running: true)).text, "Thinking…")
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .ready, running: true, lastIsAnswer: true, answerHasText: true)).text,
                       "Writing…")
        let disk = StatusWords.say(I(reachable: true, engine: .ready, running: true, flashWaitMs: 40, wallMs: 100))
        XCTAssertEqual(disk.text, "Reading part of the model from the disk…")
        XCTAssertEqual(disk.mood, .busy)
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .ready, lastIsAnswer: true)).text, "Answered")
        XCTAssertEqual(StatusWords.say(I(reachable: true, engine: .ready, lastIsAnswer: true, stopped: true)).text, "Stopped")
    }
}
