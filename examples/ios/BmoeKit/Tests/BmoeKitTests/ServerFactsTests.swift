import XCTest
@testable import BmoeKit

/// What the screens read from the server: the state, the settings schema, the models, the plan.
final class ServerFactsTests: XCTestCase {
    func testIntegersAndFloatsStayApart() throws {
        let j = try JSON(parsing: #"{"threads":8,"temp":0.7,"auto":"auto","on":true,"none":null}"#)
        XCTAssertEqual(j["threads"], .int(8))
        XCTAssertEqual(j["temp"], .double(0.7))
        XCTAssertEqual(j["on"], .bool(true))
        XCTAssertTrue(j["none"].isNull)
        XCTAssertEqual(JSON.int(8).text, "8")
        XCTAssertEqual(JSON.double(2.0).display, "2")
    }

    func testInfoReadsTheStateAndTheSession() throws {
        let info = ServerInfo(try JSON(parsing: #"""
            {"version":"0.31.0","planner_available":true,"host":{"ram_total_mib":7680,"ram_available_mib":3100,"cpu_threads":6},
             "state":"ready","error":"","generating":false,
             "model":{"path":"/m/a.gguf","name":"a.gguf","arch":"qwen3moe","n_ctx":4096,"n_expert_used":8,"think_ctl":"template",
                      "load_s":12.4,"streaming":true,"compute_threads":4,"busy_threads":6}}
            """#))
        XCTAssertTrue(info.ready)
        XCTAssertEqual(info.model?.computeThreads, 4)
        XCTAssertEqual(info.ramTotalMiB, 7680)
        let empty = ServerInfo(try JSON(parsing: #"{"state":"planning","model":null}"#))
        XCTAssertFalse(empty.ready)
        XCTAssertTrue(empty.busyLoading)
        XCTAssertNil(empty.model)
    }

    func testParamsGroupsFollowTheGroupOrderWhenItIsSent() throws {
        let specs = ParamSpec.parse(try JSON(parsing: #"""
            [{"key":"a","group":"late","group_label":"Late","group_order":2},
             {"key":"b","group":"early","group_label":"Early","group_order":0},
             {"key":"c","group":"late","group_label":"Late","group_order":2},
             {"key":"d","group":"unordered","group_label":"Unordered"}]
            """#))
        let groups = ParamSpec.grouped(specs)
        XCTAssertEqual(groups.map(\.label), ["Early", "Late", "Unordered"])
        XCTAssertEqual(groups[1].rows.map(\.key), ["a", "c"])
    }

    func testParamsGroupInTableOrderAndTypeTheirValues() throws {
        let specs = ParamSpec.parse(try JSON(parsing: #"""
            [{"key":"cache-mb","label":"Expert cache","type":"int","group":"cache","group_label":"Expert cache","level":"basic",
              "scope":"session","min":0,"max":2147483647,"unit":"MiB","accepts_auto":true,"default":"auto"},
             {"key":"temp","label":"Temperature","type":"float","group":"sampling","group_label":"Sampling","level":"basic",
              "scope":"session","lossy":true,"default":0.8},
             {"key":"overlap","label":"Overlap","type":"bool","group":"cache","group_label":"Expert cache","level":"advanced",
              "scope":"session","default":true},
             {"key":"mode","label":"Mode","type":"choice","group":"sampling","group_label":"Sampling","choices":[{"value":"a","label":"A"}]}]
            """#))
        let groups = ParamSpec.grouped(specs)
        XCTAssertEqual(groups.map(\.label), ["Expert cache", "Sampling"])
        XCTAssertEqual(groups[0].rows.map(\.key), ["cache-mb", "overlap"])
        let cache = specs[0]
        XCTAssertEqual(cache.value(fromText: "auto"), .string("auto"))
        XCTAssertEqual(cache.value(fromText: " 512 "), .int(512))
        XCTAssertNil(cache.value(fromText: "5.5"))
        XCTAssertEqual(cache.rangeText, "at least 0 MiB") // an older server's type limit is no range
        XCTAssertNil(cache.typical)
        XCTAssertNil(cache.typicalText)
        XCTAssertEqual(specs[1].value(fromText: "1"), .double(1))
        XCTAssertTrue(specs[1].lossy)
        XCTAssertEqual(specs[2].value(fromText: "true"), .bool(true))
        XCTAssertEqual(specs[2].level, .advanced)
        XCTAssertEqual(specs[3].choices, [ParamSpec.Choice(value: "a", label: "A")])
    }

    func testSpecialValuesTheTypicalRangeAndTheAdvancedRows() throws {
        let specs = ParamSpec.parse(try JSON(parsing: #"""
            [{"key":"io-threads","label":"Read lanes","type":"int","group":"streaming","group_label":"Streaming","level":"advanced",
              "help":"Parallel expert-read lanes, including the calling thread. 1 is the serial baseline.",
              "min":1,"max":16,"unit":"lanes","typical":{"min":2,"max":8},"special":[{"value":0,"label":"off"}],"default":4},
             {"key":"seed","label":"Seed","type":"int","group":"sampling","group_label":"Sampling","level":"basic",
              "min":0,"typical":{"min":3,"max":3},"special":[{"value":4294967295,"label":"random per run"},{"value":1}]},
             {"key":"temp","label":"Temperature","type":"float","group":"streaming","group_label":"Streaming","level":"basic",
              "typical":{"min":0.6,"max":1.0},"special":[{"value":0,"label":"greedy"}]},
             {"key":"ctx","label":"Context","type":"int","group":"sampling","group_label":"Sampling","level":"debug",
              "typical":{"min":9,"max":2}}]
            """#))
        let lanes = specs[0]
        XCTAssertEqual(lanes.typical, 2...8)
        XCTAssertEqual(lanes.typicalText, "usually 2 to 8")
        XCTAssertEqual(lanes.rangeText, "1 to 16 lanes")
        XCTAssertEqual(lanes.specialLabel(.int(0)), "off")
        XCTAssertEqual(lanes.specialLabel(.double(0)), "off")
        XCTAssertEqual(lanes.specialLabel(.string("0")), "off")
        XCTAssertNil(lanes.specialLabel(.int(4)))
        XCTAssertEqual(lanes.helpParts.lead, "Parallel expert-read lanes, including the calling thread.")
        XCTAssertEqual(lanes.helpParts.rest, "1 is the serial baseline.")
        let seed = specs[1]
        XCTAssertEqual(seed.special, [ParamSpec.Special(value: "4294967295", label: "random per run")]) // a nameless value is dropped
        XCTAssertEqual(seed.specialLabel(.int(4_294_967_295)), "random per run")
        XCTAssertEqual(seed.typicalText, "usually 3")
        XCTAssertEqual(seed.rangeText, "at least 0")
        XCTAssertEqual(specs[2].typicalText, "usually 0.6 to 1")
        XCTAssertEqual(specs[2].specialLabel(.double(0)), "greedy")
        XCTAssertNil(specs[3].typical) // a range upside down is no guide
        // The Advanced fold holds only what the basic groups do not, in the table's group order.
        let advanced = ParamSpec.advanced(specs)
        XCTAssertEqual(advanced.map(\.label), ["Streaming", "Sampling"])
        XCTAssertEqual(advanced.flatMap { $0.rows.map(\.key) }, ["io-threads", "ctx"])
    }

    func testHelpSplitsAtTheFirstSentenceThatStandsAlone() throws {
        func parts(_ help: String) -> (String, String) {
            let p = ParamSpec(.object(["key": "k", "help": .string(help)])).helpParts
            return (p.lead, p.rest)
        }
        XCTAssertEqual(parts("Run it on this device (e.g. HTP0) while decode stays.").1, "")
        XCTAssertEqual(parts("Compute threads.").0, "Compute threads.")
        XCTAssertEqual(parts("Short. Then a second sentence here.").1, "") // a lead under 24 characters stays whole
        XCTAssertEqual(parts("").0, "")
    }

    func testConfigKnowsWhoSetWhat() throws {
        let c = ConfigState(try JSON(parsing: #"""
            {"values":{"threads":8,"cache-mb":"auto"},"user_keys":["threads"],"args":["--threads","8"],"valid":false,
             "error":"bad","reload_required":true,"plan":[{"knob":"cache-mb","value":"703","source":"derived","reason":"r"}],
             "auto_plan":true,"rejected":{"x":"not a number"}}
            """#))
        XCTAssertEqual(c["threads"], .int(8))
        XCTAssertEqual(c.userKeys, ["threads"])
        XCTAssertFalse(c.valid)
        XCTAssertEqual(c.plan?.first?.source, "derived")
        XCTAssertTrue(c.autoPlan)
        XCTAssertEqual(c.rejected["x"], "not a number")
        XCTAssertNil(ConfigState(try JSON(parsing: #"{"values":{},"plan":null}"#)).plan)
    }

    func testCatalogStatusNeedsEveryShard() throws {
        let list = ModelsList(try JSON(parsing: #"""
            {"models_dir":"/d","scan_dirs":[],
             "local":[{"name":"m-00001-of-00002.gguf","path":"/d/m-00001-of-00002.gguf","files":["/d/m-00001-of-00002.gguf"],
                       "complete":false,"streamable":true}],
             "catalog":[{"id":"m","title":"M","status":"available","files":[{"name":"m-00001-of-00002.gguf","bytes":1,"url":"https://x/1"},
                                                                         {"name":"m-00002-of-00002.gguf","bytes":1,"url":"https://x/2"}]},
                        {"id":"empty","files":[]}]}
            """#))
        XCTAssertEqual(list.catalog.map(\.id), ["m"])
        let m = list.catalog[0]
        XCTAssertEqual(m.bytes, 2)
        XCTAssertEqual(list.status(of: m, downloading: []), .available)
        XCTAssertEqual(list.status(of: m, downloading: ["m"]), .downloading)
        XCTAssertFalse(list.local[0].complete)
    }

    func testAFileNameThatLeavesTheModelsDirDropsTheEntry() throws {
        let list = ModelsList(try JSON(parsing: #"""
            {"catalog":[{"id":"up","files":[{"name":"../m.gguf","bytes":1,"url":"https://x/m"}]},
                        {"id":"sub","files":[{"name":"a/m.gguf","bytes":1,"url":"https://x/m"}]},
                        {"id":"ok","files":[{"name":"m-00001-of-00002.gguf","bytes":1,"url":"https://x/m"}]}]}
            """#))
        XCTAssertEqual(list.catalog.map(\.id), ["ok"])
        for bad in ["..", ".", "", "a\\m.gguf", "C:m.gguf"] { XCTAssertFalse(CatalogFile.isPlainName(bad), bad) }
    }

    func testAStoppedDownloadReadsAsResume() throws {
        let list = ModelsList(try JSON(parsing: #"""
            {"catalog":[{"id":"m","bytes":47000000000,"received":14230000000,"partial":true,
                         "files":[{"name":"m.gguf","bytes":47000000000,"url":"https://x/m"}]},
                        {"id":"n","files":[{"name":"n.gguf","bytes":1,"url":"https://x/n"}]}]}
            """#))
        XCTAssertTrue(list.catalog[0].partial)
        XCTAssertEqual(list.catalog[0].received, 14_230_000_000)
        XCTAssertFalse(list.catalog[1].partial)
        XCTAssertEqual(list.catalog[1].received, 0)
        XCTAssertEqual(ByteText.resume(received: 14_230_000_000, total: 47_000_000_000), "Resume (14.2 of 47.0 GB)")
        XCTAssertEqual(ByteText.resume(received: 0, total: 1_500_000_000), "Resume (0.0 of 1.5 GB)")
    }

    func testSizesBelowAGigabyteKeepTheirDigits() {
        XCTAssertEqual(ByteText.gb(22_285_080_192), "22.3 GB")
        XCTAssertEqual(ByteText.gb(7_340_032), "7 MB") // a test model read as "0.0 GB" (#359)
        XCTAssertEqual(ByteText.gb(512_000), "512 KB")
        XCTAssertEqual(ByteText.gb(0), "0 KB")
    }

    func testTheFirstTokenIsThisTurnsNotTheLoads() throws {
        var t = Telemetry()
        // A second turn: the session loaded 55.9 s before the first one; this turn prefilled in 6.8 s.
        t.apply(done: try JSON(parsing: #"{"tok_s":2.6,"load_s":55.9,"prefill_s":6.8,"ttft_s":6.8}"#))
        XCTAssertEqual(t.ttftSeconds, 6.8, accuracy: 1e-9)
        var older = Telemetry() // a server that predates ttft_s: its prefill, still without the load
        older.apply(done: try JSON(parsing: #"{"tok_s":2.6,"load_s":55.9,"prefill_s":6.8}"#))
        XCTAssertEqual(older.ttftSeconds, 6.8, accuracy: 1e-9)
    }

    func testFilesThatAreNotModelsAreListedApart() throws {
        let list = ModelsList(try JSON(parsing: #"""
            {"local":[{"name":"mmproj-a.gguf","path":"/d/mmproj-a.gguf","role":"projector"},
                      {"name":"a.gguf","path":"/d/a.gguf","role":"model","folder":"a"},
                      {"name":"mtp-a.gguf","path":"/d/mtp-a.gguf","role":"draft"},
                      {"name":"b.gguf","path":"/d/b.gguf","role":""},
                      {"name":"c.gguf","path":"/d/c.gguf"},
                      {"name":"v.gguf","path":"/d/v.gguf","role":"other"}]}
            """#))
        let (models, companions) = FileRole.split(list.local) { $0.role }
        XCTAssertEqual(models.map(\.name), ["a.gguf", "b.gguf", "c.gguf"]) // "" is an unreadable header, not a companion
        XCTAssertEqual(companions.map(\.name), ["mmproj-a.gguf", "mtp-a.gguf", "v.gguf"])
        XCTAssertEqual(list.local[1].folder, "a")
        XCTAssertEqual(list.local[0].folder, "")
        XCTAssertEqual(companions.map { FileRole.label($0.role) }, ["projector", "draft head", "not a model"])
        XCTAssertFalse(FileRole.isCompanion("model"))
        XCTAssertEqual(FileRole.isoDate("2026-07-17T13:00:49.000Z"), "2026-07-17")
        XCTAssertEqual(FileRole.isoDate("yesterday"), "")
        XCTAssertEqual(FileRole.isoDate(""), "")
    }

    func testPlanAndItsReproduceLine() throws {
        let p = PlanResult(try JSON(parsing: #"""
            {"available":true,"model":"/m/my model.gguf","machine":"6 cores","regime":"experts-stream",
             "decisions":[{"knob":"cache-mb","value":703,"source":"derived","reason":"fits"}],"args":["--cache-mb","703"]}
            """#))
        XCTAssertEqual(p.decisions.first?.value, "703")
        XCTAssertEqual(p.reproduceLine, "bmoe-cli -m '/m/my model.gguf' --cache-mb 703")
        XCTAssertEqual(PlanResult(try JSON(parsing: #"{"available":false,"reason":"no planner"}"#)).error, "no planner")
    }

    func testHuggingFaceAnswers() throws {
        let models = HFModels(try JSON(parsing: #"""
            {"repo":"o/r","models":[{"name":"a.gguf","quant":"Q4_0","bytes":10,"count":2,"complete":false},
                                    {"name":"mmproj-a.gguf","role":"projector"}]}
            """#))
        XCTAssertEqual(models.revision, "main")
        XCTAssertEqual(models.models.first?.count, 2)
        XCTAssertEqual(models.models.map(\.role), ["model", "projector"]) // an older server sends no role: a model
        let hits = HFSearch(try JSON(parsing: #"{"repos":[{"id":"o/r","modified":"2026-07-17T13:00:49.000Z","gated":true}]}"#))
        XCTAssertEqual(hits.repos.first?.modified, "2026-07-17T13:00:49.000Z")
        let projector = HFFacts(try JSON(parsing: #"""
            {"model":{"readable":true,"arch":"clip","role":"projector"},"verdict":{"downloadable":false,"mode":"not a model"}}
            """#))
        XCTAssertEqual(projector.role, "projector")
        XCTAssertFalse(projector.downloadable)
        let facts = HFFacts(try JSON(parsing: #"""
            {"name":"a.gguf","model":{"readable":true,"arch":"gemma4","is_moe":true,"n_expert":128,"n_expert_used":8,"streamable":true},
             "verdict":{"downloadable":true,"mode":"streams","disk":"d","memory":"m"}}
            """#))
        XCTAssertTrue(facts.isMoE)
        XCTAssertEqual(facts.mode, "streams")
        XCTAssertEqual(HF.body(repo: " o/r ", token: " ")["token"], .null)
        XCTAssertEqual(HF.body(repo: "o/r", name: "a", token: "t")["name"].string, "a")
    }
}
