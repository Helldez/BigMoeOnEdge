import XCTest
@testable import BmoeKit

/// What the app keeps for itself: conversations, metrics files, and what a download still owes.
final class StorageTests: XCTestCase {
    private var dir: URL!

    override func setUpWithError() throws {
        dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: dir)
    }

    func testConversationsRoundTripNewestFirst() throws {
        let store = ConversationStore(directory: dir.appendingPathComponent("c"))
        var older = Conversation(messages: [ChatMessage(role: .user, content: "first line\nsecond")], updated: Date(timeIntervalSince1970: 1))
        let newer = Conversation(messages: [ChatMessage(role: .user, content: "hello")], updated: Date(timeIntervalSince1970: 2))
        older.messages.append(ChatMessage(role: .assistant, content: "ok", reasoning: "r", metrics: "1 tok/s"))
        try store.save(older)
        try store.save(newer)
        let list = store.list()
        XCTAssertEqual(list.map(\.id), [newer.id, older.id])
        XCTAssertEqual(list[1].title, "first line")
        XCTAssertEqual(list[1].messages.last?.reasoning, "r")
        store.delete(newer.id)
        XCTAssertEqual(store.list().count, 1)
        XCTAssertEqual(Conversation().title, "New chat")
    }

    func testRecorderWritesWhatTheReaderReads() throws {
        let rec = MetricsRecorder(directory: dir)
        let info = ServerInfo(try JSON(parsing: #"""
            {"version":"0.31.0","state":"ready","model":{"name":"q.gguf","arch":"qwen3moe","n_ctx":4096,"streaming":true,
             "compute_threads":4,"busy_threads":6}}
            """#))
        let config = ConfigState(try JSON(parsing: #"{"values":{"cache-mb":"auto","overlap":true,"model":"/q.gguf"}}"#))
        rec.sessionOpened(info: info, config: config, now: Date(timeIntervalSince1970: 0))
        rec.token(try JSON(parsing: #"{"step":1,"steps":8,"wall_ms":250.5,"cache_hit_pct":-1}"#))
        rec.token(try JSON(parsing: #"{"step":2,"steps":8,"wall_ms":200,"cache_hit_pct":40}"#))
        rec.done(try JSON(parsing: #"{"tokens":2,"tok_s":4.5}"#))
        rec.close()
        let files = try FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)
        XCTAssertEqual(files.count, 1)
        let f = try MetricsFile(contentsOf: files[0])
        XCTAssertEqual(f.info["model"], "q.gguf")
        XCTAssertEqual(f.info["cache_mb"], "auto")
        XCTAssertEqual(f.info["overlap"], "1")
        XCTAssertNil(f.info["model_path"])
        XCTAssertEqual(f.column("wall_ms"), [250.5, 200])
        XCTAssertEqual(f.column("cache_hit_pct"), [nil, 40])
        XCTAssertEqual(f.column("turn"), [1, 1])
        XCTAssertTrue(f.plottable.contains("wall_ms"))
        XCTAssertFalse(f.plottable.contains("step"))
        XCTAssertEqual(f.title, "q.gguf · 4.50 tok/s")
        XCTAssertEqual(ColumnStats(f.column("wall_ms")!)?.mean ?? 0, 225.25, accuracy: 1e-9)
    }

    func testADownloadOwesOnlyTheFilesWithoutTheirExactSize() throws {
        try Data(count: 4).write(to: dir.appendingPathComponent("a.gguf"))
        try Data(count: 2).write(to: dir.appendingPathComponent("b.gguf"))
        let files = [CatalogFile(name: "a.gguf", bytes: 4, url: "https://x/a"), CatalogFile(name: "b.gguf", bytes: 4, url: "https://x/b"),
                     CatalogFile(name: "c.gguf", bytes: 4, url: "https://x/c")]
        XCTAssertEqual(DownloadPlan.remaining(files, in: dir).map(\.name), ["b.gguf", "c.gguf"])
    }

    func testAStoppedDownloadCountsWhatLandedAndWhatItsResumeDataHolds() throws {
        let models = dir.appendingPathComponent("models"), resume = dir.appendingPathComponent("resume")
        try FileManager.default.createDirectory(at: models, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: resume, withIntermediateDirectories: true)
        let files = [CatalogFile(name: "a.gguf", bytes: 4, url: "https://x/a"), CatalogFile(name: "b.gguf", bytes: 10, url: "https://x/b")]
        try Data(count: 4).write(to: models.appendingPathComponent("a.gguf"))
        // Nothing to resume without resume data, though a shard landed whole.
        XCTAssertNil(DownloadPlan.partial(files, in: models, resumeDir: resume))
        let data = try PropertyListSerialization.data(fromPropertyList: ["NSURLSessionResumeBytesReceived": 6,
                                                                          "NSURLSessionResumeInfoTempFileName": "CFNetworkDownload_x.tmp"],
                                                      format: .binary, options: 0)
        try data.write(to: resume.appendingPathComponent("b.gguf.resume"))
        XCTAssertEqual(ResumeData.bytesReceived(data), 6)
        XCTAssertEqual(DownloadPlan.partial(files, in: models, resumeDir: resume), 10)
        // Resume data the system no longer reads the same way still says a download can resume.
        try Data("not a plist".utf8).write(to: resume.appendingPathComponent("b.gguf.resume"))
        XCTAssertEqual(DownloadPlan.partial(files, in: models, resumeDir: resume), 4)
        // The bytes it names are found in the folders the system keeps them in.
        try Data(count: 6).write(to: dir.appendingPathComponent("CFNetworkDownload_x.tmp"))
        XCTAssertEqual(ResumeData.tempFile(data, candidates: [resume, dir])?.lastPathComponent, "CFNetworkDownload_x.tmp")
        XCTAssertNil(ResumeData.tempFile(data, candidates: [resume]))
    }

    func testTheLiveCardSplitsTheWall() throws {
        var t = Telemetry()
        t.computeThreads = 4
        t.busyThreads = 4
        t.apply(token: try JSON(parsing: #"{"step":3,"wall_ms":100,"flash_wait_ms":30,"mgmt_ms":5,"cpu_ms":200}"#))
        let b = t.breakdown
        XCTAssertFalse(b.isAverage)
        XCTAssertEqual(b.computeMs, 50)
        XCTAssertEqual(b.unattributedMs, 15)
        XCTAssertEqual(b.cpuBusyPct, 50)
        XCTAssertEqual(t.rate, 10)
        t.apply(done: try JSON(parsing: #"{"tok_s":5,"flash_wait_s_tok":0.1,"mgmt_s_tok":0.0,"cpu_s_tok":0.4,"tokens":9}"#))
        XCTAssertTrue(t.breakdown.isAverage)
        XCTAssertEqual(t.breakdown.wallMs, 200)
        XCTAssertEqual(t.breakdown.computeMs, 100)
        XCTAssertEqual(t.step, 9)
    }
}
