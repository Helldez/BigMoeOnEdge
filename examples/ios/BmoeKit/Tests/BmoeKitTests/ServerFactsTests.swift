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
        XCTAssertEqual(cache.rangeText, "0 to any MiB")
        XCTAssertEqual(specs[1].value(fromText: "1"), .double(1))
        XCTAssertTrue(specs[1].lossy)
        XCTAssertEqual(specs[2].value(fromText: "true"), .bool(true))
        XCTAssertEqual(specs[2].level, .advanced)
        XCTAssertEqual(specs[3].choices, [ParamSpec.Choice(value: "a", label: "A")])
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
        let models = HFModels(try JSON(parsing: #"{"repo":"o/r","models":[{"name":"a.gguf","quant":"Q4_0","bytes":10,"count":2,"complete":false}]}"#))
        XCTAssertEqual(models.revision, "main")
        XCTAssertEqual(models.models.first?.count, 2)
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
