import XCTest
@testable import BmoeKit

/// The plan as the Plan screen draws it: every word from the server, a code only beside a word.
final class PlanTests: XCTestCase {
    /// The sample of docs/server-api.md, `GET /api/plan`.
    static let sample = #"""
        {"available": true, "model": "C:/models/Qwen3.6-35B-A3B-Q4_K_M.gguf",
         "machine": "16 cores, 3388 MiB available of 15182 MiB, 33 GiB/s on this model's matmul",
         "regime": "experts-stream", "streaming_declined": false, "decline_reason": "",
         "decisions": [{"knob": "cache-mb", "label": "Expert cache", "group": "set", "value": "703",
                        "source": "derived", "brief": "703 MiB: what the cache earns most with, after the dense weights.",
                        "reason": "..."}],
         "values": {"cache-mb": 703, "threads": 8}, "args": ["--moe-stream", "--cache-mb", "703"],
         "not_applicable": [],
         "summary": {"sentence": "Experts stream from flash, with a 0.7 GiB cache.",
                     "lanes": [{"phase": "prompt", "label": "Prompt", "on": ["CPU"]},
                               {"phase": "tokens", "label": "Tokens", "on": ["CPU"]}]},
         "predicted": {"s_per_token": 0.302, "cache_hit_pct": 38.0},
         "memory": {"pools": [{"name": "host", "label": "This process", "cap_bytes": 3552575488,
                               "lockable_cap_bytes": 0, "held_bytes": 3322400768, "locked_bytes": 0,
                               "rows": [{"name": "dense", "label": "Dense", "bytes": 1932735283,
                                         "locked": false, "in_turn": false}]}]},
         "devices": [{"name": "CPU", "description": "...", "cpu": true, "own_memory": false, "memory_total": 0,
                      "prompt_ratio": 1.0, "token_ratio": 1.0, "rate_text": "The reference",
                      "uses": ["prompt", "tokens"], "status": "used", "badge": "Prompt and tokens",
                      "why": "The host: every rate is measured against it."}],
         "devices_none": "build", "devices_none_text": "No other device: this build carries no backend for one.",
         "explain": "..."}
        """#

    func testTheDocumentedPlanReadsWhole() throws {
        let p = PlanResult(try JSON(parsing: Self.sample))
        XCTAssertTrue(p.available)
        XCTAssertEqual(p.model, "C:/models/Qwen3.6-35B-A3B-Q4_K_M.gguf")
        XCTAssertFalse(p.none)
        XCTAssertFalse(p.cached)
        XCTAssertEqual(p.sentence, "Experts stream from flash, with a 0.7 GiB cache.")
        XCTAssertEqual(p.lanes.map(\.label), ["Prompt", "Tokens"])
        XCTAssertEqual(p.lanes[1].on, ["CPU"])
        XCTAssertEqual(p.predicted?.sPerToken, 0.302)
        XCTAssertEqual(p.predicted?.cacheHitPct, 38)
        XCTAssertNil(p.measured)
        XCTAssertEqual(p.pools.count, 1)
        XCTAssertEqual(p.pools[0].label, "This process")
        XCTAssertEqual(p.pools[0].capBytes, 3_552_575_488)
        XCTAssertEqual(p.pools[0].rows, [.init(name: "dense", label: "Dense", bytes: 1_932_735_283, locked: false, inTurn: false)])
        XCTAssertEqual(p.devices.first?.badge, "Prompt and tokens")
        XCTAssertEqual(p.devices.first?.status, "used")
        XCTAssertEqual(p.devices.first?.uses, ["prompt", "tokens"])
        XCTAssertEqual(p.devices.first?.rateText, "The reference")
        XCTAssertEqual(p.devicesNoneText, "No other device: this build carries no backend for one.")
        let d = try XCTUnwrap(p.decisions.first)
        XCTAssertEqual(d.title, "Expert cache")
        XCTAssertEqual(d.group, "set")
        XCTAssertTrue(d.brief.hasPrefix("703 MiB"))
        XCTAssertEqual(d.shownValue, "703")
        let speed = p.speedCheck
        XCTAssertEqual(try XCTUnwrap(speed.expected), 1 / 0.302, accuracy: 1e-9)
        XCTAssertNil(speed.measured)
        XCTAssertNil(speed.deltaPct)
    }

    func testMeasuredTurnSetsBesideThePrediction() throws {
        let p = PlanResult(try JSON(parsing: #"""
            {"available":true,"from_last_load":true,"predicted":{"s_per_token":0.2},
             "measured":{"s_per_token":0.16,"cache_hit_pct":-1,"tokens":64}}
            """#))
        XCTAssertTrue(p.fromLastLoad)
        XCTAssertEqual(p.measured?.tokens, 64)
        XCTAssertEqual(p.predicted?.cacheHitPct, -1)
        XCTAssertEqual(try XCTUnwrap(p.speedCheck.deltaPct), 25, accuracy: 1e-9)
    }

    func testNothingHeldIsNone() throws {
        let p = PlanResult(try JSON(parsing: #"{"available":true,"none":true}"#))
        XCTAssertTrue(p.available)
        XCTAssertTrue(p.none)
        XCTAssertTrue(p.error.isEmpty)
        XCTAssertTrue(p.decisions.isEmpty && p.pools.isEmpty && p.devices.isEmpty)
        XCTAssertNil(p.predicted)
        XCTAssertTrue(PlanResult(try JSON(parsing: #"{"available":true,"cached":true}"#)).cached)
    }

    func testDecisionsGroupAndFallBack() throws {
        let all = PlanDecision.list(try JSON(parsing: #"""
            [{"knob":"cache-mb","group":"set","value":703,"value_text":"703 MiB"},
             {"knob":"threads","group":"kept","value":8},
             {"knob":"regime","label":"Streaming mode","group":"notes","value":"-"},
             {"knob":"old","value":"1"}]
            """#))
        let g = PlanDecision.grouped(all)
        XCTAssertEqual(g.set.map(\.knob), ["cache-mb", "old"])
        XCTAssertEqual(g.kept.map(\.knob), ["threads"])
        XCTAssertEqual(g.notes.map(\.title), ["Streaming mode"])
        XCTAssertEqual(all[0].shownValue, "703 MiB")
        XCTAssertEqual(all[1].title, "threads")
        XCTAssertEqual(all[2].shownValue, "")
    }

    func testProgressEventsAndTheirEnd() throws {
        let p = try XCTUnwrap(PlanProgress(try JSON(parsing: #"{"step":"storage","label":"Storage speed","index":3,"count":7}"#)))
        XCTAssertEqual(p.label, "Storage speed")
        XCTAssertEqual(p.left, 3)
        XCTAssertEqual(p.fraction, 3.0 / 7.0, accuracy: 1e-9)
        XCTAssertEqual(PlanProgress(try JSON(parsing: #"{"step":"model","index":1,"count":7}"#))?.label, "model")
        XCTAssertNil(PlanProgress(try JSON(parsing: #"{"step":"done"}"#)))
        XCTAssertNil(PlanProgress(try JSON(parsing: #"{}"#)))
    }
}
