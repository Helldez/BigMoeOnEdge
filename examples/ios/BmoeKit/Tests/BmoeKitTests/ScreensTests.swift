import XCTest
@testable import BmoeKit

/// What the screens build from the server's answers before they draw: the one models list, the
/// context warning, the settings cards, the plan's figures, and the check a download passes.
final class ScreensTests: XCTestCase {
    private var dir: URL!

    override func setUpWithError() throws {
        dir = FileManager.default.temporaryDirectory.appendingPathComponent("bmoekit-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: dir)
    }

    // MARK: the download's check

    // FIPS 180-2's own vectors.
    static let abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    static let empty = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    static let million = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0" // a million "a"

    func testTheDigestIsStreamedInChunks() throws {
        let f = dir.appendingPathComponent("a")
        try Data("abc".utf8).write(to: f)
        XCTAssertEqual(try Checksum.sha256Hex(of: f), Self.abc)
        try Data().write(to: f)
        XCTAssertEqual(try Checksum.sha256Hex(of: f), Self.empty)
        try Data(repeating: UInt8(ascii: "a"), count: 1_000_000).write(to: f)
        // A chunk that divides nothing evenly: the digest does not depend on where the reads fall.
        XCTAssertEqual(try Checksum.sha256Hex(of: f, chunkBytes: 4093), Self.million)
        XCTAssertThrowsError(try Checksum.sha256Hex(of: f, chunkBytes: 4093) { true }) { XCTAssertTrue($0 is CancellationError) }
    }

    func testAMatchingFileLandsAndAMismatchIsDeleted() throws {
        let part = dir.appendingPathComponent("m.gguf.part"), dest = dir.appendingPathComponent("m.gguf")
        try Data("abc".utf8).write(to: part)
        try Checksum.land(part, at: dest, file: "m.gguf", sha256: Self.abc)
        XCTAssertEqual(try Data(contentsOf: dest), Data("abc".utf8))
        XCTAssertFalse(FileManager.default.fileExists(atPath: part.path))

        try Data("abd".utf8).write(to: part)
        XCTAssertThrowsError(try Checksum.land(part, at: dest, file: "m.gguf", sha256: Self.abc)) { e in
            let m = e as? Checksum.Mismatch
            XCTAssertEqual(m?.expected, Self.abc)
            XCTAssertTrue(e.localizedDescription.contains("ba7816bf8f01"), e.localizedDescription)
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: part.path)) // resuming it would bring back the same bytes
        XCTAssertEqual(try Data(contentsOf: dest), Data("abc".utf8)) // the model that was there stays

        try Data("xyz".utf8).write(to: part)
        try Checksum.land(part, at: dest, file: "m.gguf", sha256: "") // no digest named: taken as it came
        XCTAssertEqual(try Data(contentsOf: dest), Data("xyz".utf8))
    }

    func testAWholePartIsCheckedOnTheNextStartAndAnUnknownSizeIsThereOnceItIs() throws {
        let known = CatalogFile(name: "k.gguf", bytes: 3, url: "https://x/k")
        let unknown = CatalogFile(name: "u.gguf", bytes: 0, url: "https://x/u")
        XCTAssertEqual(DownloadPlan.remaining([known, unknown], in: dir).map(\.name), ["k.gguf", "u.gguf"])
        try Data("abc".utf8).write(to: DownloadPlan.partURL("k.gguf", in: dir))
        XCTAssertTrue(DownloadPlan.uncheckedPart(known, in: dir))
        XCTAssertEqual(DownloadPlan.partial([known], in: dir, resumeDir: dir), 3)
        try Data("u".utf8).write(to: dir.appendingPathComponent("u.gguf"))
        XCTAssertEqual(DownloadPlan.remaining([known, unknown], in: dir).map(\.name), ["k.gguf"])
    }

    func testEachCheckReadsACopyOfItsOwnAndOneCutOffIsFoundAgain() throws {
        let known = CatalogFile(name: "k.gguf", bytes: 3, url: "https://x/k")
        let unknown = CatalogFile(name: "u.gguf", bytes: 0, url: "https://x/u")
        let a = DownloadPlan.checkURL("k.gguf", in: dir), b = DownloadPlan.checkURL("k.gguf", in: dir)
        XCTAssertNotEqual(a, b) // two copies finishing together never share the name a check reads
        try Data("ab".utf8).write(to: a) // not whole: not a copy to check
        XCTAssertFalse(DownloadPlan.uncheckedPart(known, in: dir))
        try Data("abc".utf8).write(to: b) // whole, left by a check the app's end cut off
        XCTAssertEqual(DownloadPlan.uncheckedParts(known, in: dir), [b])
        // A link file whose size nobody said: any copy with bytes in it came whole.
        try Data("u".utf8).write(to: DownloadPlan.partURL("u.gguf", in: dir))
        XCTAssertTrue(DownloadPlan.uncheckedPart(unknown, in: dir))
        // The copy hashed is the copy landed, whatever lands at the shared partial name meanwhile.
        try Data("abd".utf8).write(to: DownloadPlan.partURL("k.gguf", in: dir))
        try Checksum.land(b, at: dir.appendingPathComponent("k.gguf"), file: "k.gguf", sha256: Self.abc)
        XCTAssertEqual(try Data(contentsOf: dir.appendingPathComponent("k.gguf")), Data("abc".utf8))
    }

    func testAStartIsRefusedWhenWhatItNeedsDoesNotFit() throws {
        let files = [CatalogFile(name: "a.gguf", bytes: 3_000_000_000, url: "https://x/a"),
                     CatalogFile(name: "b.gguf", bytes: 4, url: "https://x/b")]
        XCTAssertEqual(DownloadPlan.bytesNeeded(files, in: dir, resumeDir: dir), 3_000_000_004)
        try Data("abcd".utf8).write(to: dir.appendingPathComponent("b.gguf"))
        XCTAssertEqual(DownloadPlan.bytesNeeded(files, in: dir, resumeDir: dir), 3_000_000_000)
        XCTAssertEqual(DownloadPlan.spaceRefusal(needed: 3_000_000_000, free: 1_000_000_000), "needs 3.0 GB more, only 1.0 GB free")
        XCTAssertNil(DownloadPlan.spaceRefusal(needed: 3, free: 3))
        XCTAssertNil(DownloadPlan.spaceRefusal(needed: 3, free: nil)) // unreadable: the transfer says
        XCTAssertEqual(DownloadPlan.bytesNeeded([CatalogFile(name: "u.gguf", bytes: 0, url: "https://x/u")], in: dir, resumeDir: dir), 0)
    }

    func testTheCatalogDropsWhatTheAppCannotWriteFetchOrCheck() throws {
        let upper = Self.abc.uppercased()
        let list = ModelsList(try JSON(parsing: #"""
            {"catalog":[{"id":"ok","files":[{"name":"a.gguf","bytes":3,"url":"https://x/a","sha256":"\#(upper)"}]},
                        {"id":"none","files":[{"name":"b.gguf","bytes":3,"url":"https://x/b"}]},
                        {"id":"short","files":[{"name":"c.gguf","bytes":3,"url":"https://x/c","sha256":"abc"}]},
                        {"id":"http","files":[{"name":"d.gguf","bytes":3,"url":"http://x/d"}]},
                        {"id":"half","files":[{"name":"e-00001-of-00002.gguf","bytes":3,"url":"https://x/1"},
                                              {"name":"e-00002-of-00002.gguf","bytes":3,"url":"ftp://x/2"}]},
                        {"id":"nohost","files":[{"name":"f.gguf","bytes":3,"url":"https:///f"}]}]}
            """#))
        XCTAssertEqual(list.catalog.map(\.id), ["ok", "none"])
        XCTAssertEqual(list.catalog[0].files[0].sha256, Self.abc) // read lowercased
        XCTAssertEqual(list.catalog[1].files[0].sha256, "")
        XCTAssertTrue(CatalogFile.isSha256Hex(Self.abc))
        XCTAssertFalse(CatalogFile.isSha256Hex(upper))
        // A task tagged by a build from before the digest still decodes.
        let old = try JSONDecoder().decode(CatalogFile.self, from: Data(#"{"name":"a.gguf","bytes":3,"url":"https://x/a"}"#.utf8))
        XCTAssertEqual(old.sha256, "")
    }

    // MARK: the one models list

    private func models() throws -> ModelsList {
        ModelsList(try JSON(parsing: #"""
            {"models_dir":"/d","scan_dirs":["/s"],
             "local":[{"name":"a.gguf","path":"/d/a.gguf","dir":"/d","files":["/d/a.gguf"]},
                      {"name":"b.gguf","path":"/d/b.gguf","dir":"/d","files":["/d/b.gguf"]},
                      {"name":"c-00001-of-00002.gguf","path":"/d/c/c-00001-of-00002.gguf","dir":"/d","folder":"c",
                       "files":["/d/c/c-00001-of-00002.gguf"],"complete":false},
                      {"name":"s.gguf","path":"/s/s.gguf","dir":"/s","files":["/s/s.gguf"]}],
             "catalog":[{"id":"A","title":"Model A","status":"on_disk","files":[{"name":"a.gguf","bytes":1,"url":"https://x/a"}]},
                        {"id":"N","title":"New","files":[{"name":"n.gguf","bytes":1,"url":"https://x/n"}]},
                        {"id":"P","title":"Stopped","files":[{"name":"p.gguf","bytes":1,"url":"https://x/p"}]},
                        {"id":"D","title":"Coming","files":[{"name":"q.gguf","bytes":1,"url":"https://x/q"}]},
                        {"id":"H","title":"Added","added":true,"files":[{"name":"h.gguf","bytes":1,"url":"https://x/h"}]}]}
            """#))
    }

    func testLocalFilesAndTheCatalogMergeByFile() throws {
        let list = try models()
        let rows = Library.rows(list, local: list.local, downloading: ["D"], partials: ["P": 1], loadedPath: "/d/b.gguf")
        XCTAssertEqual(rows.map(\.id), ["/d/a.gguf", "/d/b.gguf", "/d/c/c-00001-of-00002.gguf", "/s/s.gguf",
                                        "catalog:P", "catalog:D", "catalog:H", "catalog:N"])
        XCTAssertEqual(rows.map(\.state), [.onDevice, .loaded, .incomplete, .onDevice, .partial, .downloading, .available, .available])
        XCTAssertEqual(rows[0].title, "Model A") // the entry that made it, folded in: one row, not two
        XCTAssertEqual(rows[0].entry?.id, "A")
        XCTAssertEqual(rows.map(\.deleteName), ["a.gguf", "b.gguf", "c/c-00001-of-00002.gguf", nil, nil, nil, nil, nil])
        // Suggestions are the catalog's own entries with nothing started; an added one is the person's.
        XCTAssertEqual(rows.filter(\.suggested).map(\.id), ["catalog:N"])
        XCTAssertEqual(rows[4].partialBytes, 1)
        // A transfer in flight outranks the file's being there.
        let busy = Library.rows(list, local: list.local, downloading: ["A"], partials: [:], loadedPath: nil)
        XCTAssertEqual(busy[0].state, .downloading)
    }

    func testAnEntryIsOnTheDeviceAsTheServerSaysNotByAFileOfTheSameNameElsewhere() throws {
        let list = ModelsList(try JSON(parsing: #"""
            {"models_dir":"/d",
             "local":[{"name":"n.gguf","path":"/s/n.gguf","dir":"/s","files":["/s/n.gguf"]},
                      {"name":"o.gguf","path":"/d/f/o.gguf","dir":"/d","folder":"f","files":["/d/f/o.gguf"]}],
             "catalog":[{"id":"N","status":"available","files":[{"name":"n.gguf","bytes":1,"url":"https://x/n"}]},
                        {"id":"O","status":"on_disk","files":[{"name":"o.gguf","bytes":1,"url":"https://x/o"}]}]}
            """#))
        XCTAssertEqual(list.status(of: list.catalog[0], downloading: []), .available)
        XCTAssertEqual(list.status(of: list.catalog[1], downloading: []), .onDevice)
        let rows = Library.rows(list, local: list.local, downloading: [], partials: [:], loadedPath: nil)
        // The scan directory's copy is its own row; the entry stays a suggestion with its Download.
        XCTAssertEqual(rows.map(\.id), ["/s/n.gguf", "/d/f/o.gguf", "catalog:O", "catalog:N"])
        XCTAssertEqual(rows.last?.state, .available)
    }

    func testRunningDownloadsAtAGlance() {
        XCTAssertNil(Library.downloadSummary([:]))
        let one = Library.downloadSummary(["a": DownloadProgress(entryID: "a", received: 42, total: 100, state: .running),
                                           "b": DownloadProgress(entryID: "b", received: 1, total: 2, state: .failed("x"))])
        XCTAssertEqual(one?.count, 1)
        XCTAssertEqual(Library.downloadText(count: one!.count, fraction: one!.fraction), "Downloading 42%")
        let two = Library.downloadSummary(["a": DownloadProgress(entryID: "a", received: 50, total: 100, state: .running),
                                           "b": DownloadProgress(entryID: "b", received: 5, total: 0, state: .running)])
        XCTAssertEqual(Library.downloadText(count: two!.count, fraction: two!.fraction), "2 downloads 50%")
        XCTAssertEqual(Library.downloadText(count: 1, fraction: nil), "Downloading")
    }

    func testTheVerdictIsColouredByItsCode() throws {
        XCTAssertEqual(FitTone(fit: "fits"), .ok)
        XCTAssertEqual(FitTone(fit: "no_streaming"), .warn)
        for code in ["will_not_fit", "no_disk", "not_a_model"] { XCTAssertEqual(FitTone(fit: code), .err) }
        XCTAssertEqual(FitTone(fit: "unknown"), .none)
        XCTAssertEqual(FitTone(fit: "a code from later"), .none)
        let f = HFFacts(try JSON(parsing: #"""
            {"model":{"readable":true,"n_moe_layer":30,"token_cycle_bytes":7},
             "verdict":{"mode":"streams","fit":"will_not_fit","headline":"Will not fit here"}}
            """#))
        XCTAssertEqual(f.headline, "Will not fit here")
        XCTAssertEqual(f.tone, .err) // the code, not the mode
        XCTAssertEqual(f.nMoeLayer, 30)
        XCTAssertEqual(HFFacts(try JSON(parsing: #"{"verdict":{"mode":"streams"}}"#)).tone, .ok) // an older server
    }

    // MARK: the context warning

    func testTheContextWarningIsTheServersAdvice() throws {
        let params = ParamSpec.parse(try JSON(parsing: #"[{"key":"ctx-size","label":"Context"}]"#))
        let info = ServerInfo(try JSON(parsing: #"""
            {"state":"ready","model":{"name":"m","n_ctx":4096,"ctx_advice":{"recommended":16384,"short":true,"param":"ctx-size"}}}
            """#))
        let a = try XCTUnwrap(ContextAdvice(model: info.model, params: params))
        XCTAssertTrue(a.short)
        XCTAssertEqual(a.label, "Context")
        XCTAssertEqual(a.key, "ctx-size")
        XCTAssertTrue(a.warning.contains("4096") && a.warning.contains("16384") && a.warning.contains("expert cache"))
        XCTAssertTrue(a.fullText.contains("raise Context"))
        XCTAssertEqual(ContextAdvice(model: info.model, params: [])?.label, "ctx-size") // no row: the key as sent
        XCTAssertNil(ContextAdvice(model: ServerInfo(try JSON(parsing: #"{"state":"ready","model":{"n_ctx":4096}}"#)).model,
                                   params: params))
        XCTAssertNil(ContextAdvice(model: nil, params: params))
    }

    // MARK: the settings cards

    private func table() throws -> [ParamSpec] {
        ParamSpec.parse(try JSON(parsing: #"""
            [{"key":"model","label":"Model","type":"path","group":"model","group_label":"Model","group_order":0,
              "group_read_only":true,"level":"basic"},
             {"key":"threads","label":"Threads","type":"int","group":"compute","group_label":"Compute","group_order":2,
              "group_help":"Who computes.","level":"basic","default":4,"help":"Compute threads, for every layer."},
             {"key":"cache-mb","label":"Expert cache","type":"int","group":"cache","group_label":"Cache","group_order":1,
              "level":"basic","default":"auto","unit":"MiB"},
             {"key":"lanes","label":"Read lanes","type":"int","group":"compute","group_label":"Compute","group_order":2,
              "level":"experimental","default":1,"flag":"--io-threads"},
             {"key":"trace","label":"Trace","type":"bool","group":"compute","group_label":"Compute","group_order":2,
              "level":"debug","default":false},
             {"key":"spin","label":"Spin","type":"bool","group":"compute","group_label":"Compute","group_order":2,
              "level":"advanced","default":false},
             {"key":"probe","label":"Probe","type":"bool","group":"debugonly","group_label":"Debug only","group_order":3,
              "level":"debug","default":false}]
            """#))
    }

    func testOneCardPerGroupWithItsDeeperRowsFolded() throws {
        let params = try table()
        let all = SettingsForm.cards(params, upTo: .debug)
        XCTAssertEqual(all.map(\.group), ["model", "cache", "compute", "debugonly"])
        XCTAssertTrue(all[0].readOnly)
        XCTAssertEqual(all[2].help, "Who computes.")
        XCTAssertEqual(all[2].basic.map(\.key), ["threads"])
        XCTAssertEqual(all[2].more.map(\.level), [.advanced, .experimental, .debug]) // by level, not table order
        XCTAssertEqual(all[2].moreCount, 3)

        let basic = SettingsForm.cards(params, upTo: .basic)
        XCTAssertEqual(basic.map(\.group), ["model", "cache", "compute"]) // a card with nothing left goes
        XCTAssertEqual(basic[2].hidden, 3)
        // A row someone or the plan set is shown whatever the filter.
        let pinned = SettingsForm.cards(params, upTo: .basic, pinned: ["trace"])
        XCTAssertEqual(pinned[2].more.map(\.level), [.debug])
        XCTAssertEqual(pinned[2].hidden, 2)
        // A search reads every level, by label, help and flag.
        XCTAssertEqual(SettingsForm.cards(params, upTo: .basic, query: "io-threads").flatMap { $0.more.flatMap(\.rows) }.map(\.key),
                       ["lanes"])
        XCTAssertEqual(SettingsForm.cards(params, upTo: .basic, query: "every LAYER").flatMap(\.basic).map(\.key), ["threads"])
        XCTAssertEqual(SettingsForm.cards(params, upTo: .debug, query: "nothing like it"), [])

        XCTAssertEqual(SettingsForm.levelCounts(params), [.basic: 3, .advanced: 1, .experimental: 1, .debug: 2])
        XCTAssertEqual(SettingsForm.widened(.basic, toShow: .debug), .debug)
        XCTAssertEqual(SettingsForm.widened(.experimental, toShow: .advanced), .experimental)
    }

    func testALoadTargetIsReadFromTheReadOnlyGroupNotByName() throws {
        let params = try table()
        let none = ConfigState(try JSON(parsing: #"{"values":{"model":""}}"#))
        let set = ConfigState(try JSON(parsing: #"{"values":{"model":"/d/a.gguf"}}"#))
        let loaded = ServerInfo(try JSON(parsing: #"{"state":"ready","model":{"name":"a.gguf"}}"#))
        XCTAssertFalse(SettingsForm.loadTargetChosen(params, config: none, info: nil))
        XCTAssertTrue(SettingsForm.loadTargetChosen(params, config: set, info: nil))
        XCTAssertTrue(SettingsForm.loadTargetChosen(params, config: none, info: loaded))
        XCTAssertFalse(SettingsForm.loadTargetChosen(params, config: nil, info: nil))
        // A row outside a read-only group holding a value is no load target.
        XCTAssertFalse(SettingsForm.loadTargetChosen(params.filter { !$0.groupReadOnly }, config: set, info: nil))
    }

    func testALevelFromLaterIsFoldedAwayNotShownAsBasic() throws {
        let specs = ParamSpec.parse(try JSON(parsing: #"""
            [{"key":"a","group":"g","level":"basic"},{"key":"b","group":"g","level":"from-later"},{"key":"c","group":"g"}]
            """#))
        XCTAssertEqual(specs.map(\.level), [.basic, .debug, .basic])
        XCTAssertEqual(SettingsForm.cards(specs, upTo: .experimental).first?.hidden, 1)
    }

    func testWhereAValueCameFromAndWhatResetAllRemoves() throws {
        let params = try table()
        let c = ConfigState(try JSON(parsing: #"""
            {"values":{"model":"/d/a.gguf","threads":6,"cache-mb":"auto","lanes":1,"trace":true,"spin":false},
             "user_keys":["model","threads","spin","gone"],"launch_keys":["spin"],"reload_keys":["threads"],
             "plan":[{"knob":"threads","value":"8","group":"set"},{"knob":"cache-mb","value":"700","group":"set"},
                     {"knob":"lanes","value":"1","group":"kept"}]}
            """#))
        XCTAssertEqual(c.reloadKeys, ["threads"])
        XCTAssertEqual(c.launchKeys, ["spin"])
        // Not the launch flag, not the model a load chose: the person's own values.
        XCTAssertEqual(SettingsForm.resettable(c, params: params), ["threads", "gone"])
        XCTAssertEqual(SettingsForm.pinned(params, config: c), ["model", "threads", "cache-mb", "lanes", "trace", "spin"])

        let plan = Dictionary(uniqueKeysWithValues: (c.plan ?? []).map { ($0.knob, $0) })
        XCTAssertEqual(SettingsForm.source(userSet: true, decision: plan["threads"]), .you)
        XCTAssertEqual(SettingsForm.source(userSet: false, decision: plan["cache-mb"]), .plan)
        XCTAssertEqual(SettingsForm.source(userSet: false, decision: plan["lanes"]), .standard) // a kept row is not the plan's
        XCTAssertEqual(SettingsForm.Source.plan.text, "from the plan")
        XCTAssertTrue(SettingsForm.differsFromPlan(c["threads"], plan["threads"]))
        XCTAssertFalse(SettingsForm.differsFromPlan(.int(8), plan["threads"]))
        XCTAssertFalse(SettingsForm.differsFromPlan(.int(1), plan["lanes"]))
        XCTAssertTrue(SettingsForm.isDefault(params[2], .string("auto")))
        XCTAssertTrue(SettingsForm.isDefault(params[1], .double(4)))
        XCTAssertFalse(SettingsForm.isDefault(params[1], .int(6)))
        XCTAssertTrue(JSON.int(8).sameValue(as: .string(" 8")))
        XCTAssertFalse(JSON.string("auto").sameValue(as: .int(0)))
        XCTAssertEqual(SettingsForm.levelHint(.basic), "")
        XCTAssertFalse(SettingsForm.levelHint(.debug).isEmpty)
    }

    // MARK: the plan's figures

    func testTheFourFiguresAndWhatApplyChanged() throws {
        let p = PlanResult(try JSON(parsing: #"""
            {"available":true,"model":"/m/a.gguf","args":["--cache-mb","703"],"explain":"  the account \n",
             "predicted":{"s_per_token":0.4,"cache_hit_pct":38.0,"s_per_token_worst":1.69},
             "measured":{"s_per_token":0.3125,"cache_hit_pct":41.5,"tokens":9},
             "memory":{"pools":[{"name":"gpu","cap_bytes":9,"held_bytes":1},
                                {"name":"host","cap_bytes":4000,"held_bytes":3000}]}}
            """#))
        let f = p.figures
        XCTAssertEqual(f.expectedTokS!, 2.5, accuracy: 1e-9)
        XCTAssertEqual(f.measuredTokS!, 3.2, accuracy: 1e-9)
        XCTAssertEqual(f.deltaPct!, 28, accuracy: 1e-9)
        XCTAssertEqual(f.creditedHitPct, 38)
        XCTAssertEqual(f.measuredHitPct, 41.5)
        XCTAssertEqual(f.heldBytes, 3000) // the host pool, wherever it sits
        XCTAssertEqual(f.capBytes, 4000)
        XCTAssertEqual(f.worstMs!, 1690, accuracy: 1e-9)
        XCTAssertEqual(f.expectedMs!, 400, accuracy: 1e-9)
        XCTAssertEqual(p.diagnostic, "bmoe-cli -m /m/a.gguf --cache-mb 703\n\nthe account\n")

        let none = PlanResult(try JSON(parsing: #"{"available":true,"measured":{"s_per_token":0,"cache_hit_pct":-1}}"#)).figures
        XCTAssertNil(none.expectedTokS)
        XCTAssertNil(none.measuredHitPct)
        XCTAssertNil(none.heldBytes)
        XCTAssertNil(none.worstMs)
        XCTAssertEqual(PlanResult(try JSON(parsing: #"{"available":true}"#)).diagnostic,
                       "(no arguments: the plan declines streaming)\n")

        let params = ParamSpec.parse(try JSON(parsing: #"""
            [{"key":"cache-mb","label":"Expert cache","type":"int","unit":"MiB","special":[{"value":0,"label":"off"}]},
             {"key":"mode","label":"Mode","type":"choice","choices":[{"value":"pin","label":"pinned in memory"}]},
             {"key":"threads","label":"Threads","type":"int"}]
            """#))
        let changes = AppliedChange.between(["cache-mb": "auto", "mode": "map", "threads": 8],
                                            ["cache-mb": 703, "mode": "pin", "threads": 8.0, "extra": true], params: params)
        XCTAssertEqual(changes.map(\.key), ["cache-mb", "mode", "extra"])
        XCTAssertEqual(changes[0].from, "auto")
        XCTAssertEqual(changes[0].to, "703 MiB")
        XCTAssertEqual(changes[1].to, "pinned in memory")
        XCTAssertEqual(changes[2].label, "extra")
        XCTAssertEqual(AppliedChange.between(["cache-mb": 703], ["cache-mb": 0], params: params)[0].to, "off")
    }
}
