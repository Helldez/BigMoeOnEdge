import XCTest
@testable import BmoeKit

/// The agent's client side: the shapes of docs/agent.md as the server sends them, the transcript
/// the events build, the run summary, the schedule in words, and the move of the conversations
/// this app used to keep to the server.
final class AgentTests: XCTestCase {
    private func json(_ s: String) throws -> JSON { try JSON(parsing: s) }

    // MARK: decoding the contract

    func testDecodesConversationListAndTimes() throws {
        let list = ConversationSummary.list(try json(#"""
            [{"id":"c1","title":"Fix the build","workspace":"w1","mode":"read_only","route":"p/m","state":"waiting",
              "updated":1761580800,"task":"t1"},
             {"id":"c2","updated":"2026-10-27T10:00:00Z"},
             {"id":"c3","updated":1761580800000}]
            """#))
        XCTAssertEqual(list.count, 3)
        XCTAssertEqual(list[0].mode, .readOnly)
        XCTAssertEqual(list[0].route, "p/m")
        XCTAssertEqual(list[0].state, "waiting")
        XCTAssertTrue(list[0].busy)
        XCTAssertEqual(list[0].task, "t1")
        XCTAssertEqual(list[0].updated, Date(timeIntervalSince1970: 1_761_580_800))
        XCTAssertEqual(list[2].updated, Date(timeIntervalSince1970: 1_761_580_800), "milliseconds read as such")
        XCTAssertNotNil(list[1].updated)
        // Missing fields read as their defaults.
        XCTAssertEqual(list[1].mode, .ask)
        XCTAssertEqual(list[1].route, "local")
        XCTAssertEqual(list[1].state, "idle")
        XCTAssertEqual(list[1].displayTitle, "New chat")
    }

    func testDecodesEveryItemKind() throws {
        let c = AgentConversation(try json(#"""
            {"id":"c","title":"t","items":[
              {"id":"1","kind":"user","text":"hi"},
              {"id":"2","kind":"assistant","text":"**ok**","reasoning":"hmm","streaming":true},
              {"id":"3","kind":"tool","tool":"edit_file","title":"Edit a.txt","args":{"path":"a.txt"},"state":"waiting",
               "result":"","diff":"-a\n+b","file":"a.txt"},
              {"id":"4","kind":"helper","title":"Look it up","state":"done","summary":"found","conversation":"h1"},
              {"id":"5","kind":"notice","text":"No web here","level":"warn"},
              {"id":"6","kind":"later"}]}
            """#))
        XCTAssertEqual(c.items.map(\.kind), [.user, .assistant, .tool, .helper, .notice, .other])
        XCTAssertEqual(c.items[1].reasoning, "hmm")
        XCTAssertTrue(c.items[1].streaming)
        XCTAssertEqual(c.items[2].args["path"].string, "a.txt")
        XCTAssertEqual(c.items[2].diff, "-a\n+b")
        XCTAssertEqual(c.items[2].file, "a.txt")
        XCTAssertTrue(c.items[2].isWaiting)
        XCTAssertEqual(c.items[3].conversation, "h1")
        XCTAssertEqual(c.items[3].summary, "found")
        XCTAssertEqual(c.items[4].level, "warn")
    }

    func testDecodesWorkspacesSkillsTasksProviders() throws {
        let w = Workspace.list(try json(#"[{"id":"w","name":"Notes","path":"/d/Notes"}]"#))
        XCTAssertEqual(w, [Workspace(id: "w", name: "Notes", path: "/d/Notes")])

        let s = Skill.list(try json(#"[{"name":"tidy-notes","title":"Tidy","description":"d","source":"bundled","enabled":false,"body":"b"}]"#))
        XCTAssertEqual(s[0].source, "bundled")
        XCTAssertFalse(s[0].enabled)
        XCTAssertFalse(s[0].editable)
        XCTAssertTrue(Skill.validName("tidy-notes2"))
        XCTAssertFalse(Skill.validName("Tidy notes"))
        XCTAssertFalse(Skill.validName("-x"))
        XCTAssertFalse(Skill.validName("a--b"))

        let t = AgentTask.list(try json(#"""
            [{"id":"t","title":"Digest","prompt":"Summarise","workspace":"w","mode":"full","route":"local",
              "schedule":{"kind":"weekly","at":"08:30","every_minutes":0,"weekdays":[1,3]},"enabled":true,
              "last_run":{"at":1761580800,"conversation":"c9","outcome":"done"},"next_run":"2026-10-28T08:30:00Z"},
             {"id":"u","schedule":{"kind":"interval","every_minutes":30}}]
            """#))
        XCTAssertEqual(t[0].schedule, TaskSchedule(kind: .weekly, at: "08:30", everyMinutes: 0, weekdays: [1, 3]))
        XCTAssertEqual(t[0].mode, .full)
        XCTAssertEqual(t[0].lastRun?.conversation, "c9")
        XCTAssertNotNil(t[0].nextRun)
        XCTAssertNil(t[1].lastRun)
        XCTAssertEqual(t[1].mode, .readOnly, "a task is read only unless the person picked another mode")
        XCTAssertEqual(AgentTask().mode, .readOnly)
        let body = t[0].body
        XCTAssertEqual(body["schedule"]["weekdays"], [1, 3])
        XCTAssertEqual(body["mode"].string, "full")
        XCTAssertNil(body["id"].string, "the id travels in the path")

        let p = Provider.list(try json(#"[{"id":"or","name":"Router","base_url":"https://x/v1","models":["a","b"],"has_key":true}]"#))
        XCTAssertTrue(p[0].hasKey)
        XCTAssertEqual(p[0].routes.map(\.route), ["or/a", "or/b"])
        XCTAssertEqual(AgentRoute.label("or/b", providers: p), "Router · b")
        XCTAssertEqual(AgentRoute.label("local", providers: p), "This phone")
    }

    // MARK: the transcript

    func testItemUpsertsInPlaceAndAppendsInOrder() throws {
        var t = Transcript(conversation: "c")
        XCTAssertTrue(t.apply(event: "item", data: try json(#"{"conversation":"c","item":{"id":"1","kind":"user","text":"hi"}}"#)))
        t.apply(event: "item", data: try json(#"{"conversation":"c","item":{"id":"2","kind":"tool","tool":"read_file","state":"running"}}"#))
        t.apply(event: "item", data: try json(#"{"conversation":"c","item":{"id":"3","kind":"assistant","text":""}}"#))
        t.apply(event: "item", data: try json(#"{"conversation":"c","item":{"id":"2","kind":"tool","tool":"read_file","state":"done","result":"x"}}"#))
        XCTAssertEqual(t.items.map(\.id), ["1", "2", "3"], "a changed item stays where it first appeared")
        XCTAssertEqual(t.items[1].state, "done")
        XCTAssertEqual(t.items[1].result, "x")
    }

    func testDeltaAppendsAndStartsAnUnknownItem() throws {
        var t = Transcript(conversation: "c", items: [AgentItem(id: "a", kind: .assistant, text: "Hel", streaming: true)])
        t.apply(event: "delta", data: try json(#"{"conversation":"c","item":"a","text":"lo"}"#))
        t.apply(event: "delta", data: try json(#"{"conversation":"c","item":"a","reasoning":"think"}"#))
        XCTAssertEqual(t.items[0].text, "Hello")
        XCTAssertEqual(t.items[0].reasoning, "think")
        t.apply(event: "delta", data: try json(#"{"conversation":"c","item":"b","text":"new"}"#))
        XCTAssertEqual(t.items.last, AgentItem(id: "b", kind: .assistant, text: "new", streaming: true))
        // The whole item, when it lands, replaces what the deltas built.
        t.apply(event: "item", data: try json(#"{"conversation":"c","item":{"id":"b","kind":"assistant","text":"new!","streaming":false}}"#))
        XCTAssertEqual(t.items.last?.text, "new!")
        XCTAssertEqual(t.items.last?.streaming, false)
    }

    func testOtherConversationsEventsAreIgnored() throws {
        var t = Transcript(conversation: "c")
        XCTAssertFalse(t.apply(event: "item", data: try json(#"{"conversation":"d","item":{"id":"1","kind":"user","text":"x"}}"#)))
        XCTAssertFalse(t.apply(event: "delta", data: try json(#"{"conversation":"d","item":"1","text":"x"}"#)))
        XCTAssertFalse(t.apply(event: "state", data: try json(#"{"conversation":"c"}"#)))
        XCTAssertTrue(t.items.isEmpty)
    }

    func testTruncateDropsTheItemAndWhatFollows() {
        var t = Transcript(conversation: "c", items: ["1", "2", "3"].map { AgentItem(id: $0, kind: .user) })
        t.truncate(from: "2")
        XCTAssertEqual(t.items.map(\.id), ["1"])
    }

    func testConversationEventsUpdateTheList() throws {
        var list = [ConversationSummary(id: "a", title: "old", state: "idle")]
        let row = ConversationList.apply(&list, event: try json(#"{"id":"a","title":"new","state":"running","updated":10}"#))
        XCTAssertEqual(row?.state, "running")
        XCTAssertEqual(list[0].title, "new")
        XCTAssertEqual(list[0].updated, Date(timeIntervalSince1970: 10))
        ConversationList.apply(&list, event: try json(#"{"id":"b","title":"from a task","state":"running"}"#))
        XCTAssertEqual(list.map(\.id), ["b", "a"])
    }

    // MARK: the run summary

    private func tool(_ id: String, _ name: String, _ state: String = "done") -> AgentItem {
        AgentItem(id: id, kind: .tool, tool: name, title: name, state: state)
    }

    func testRunSummaryCountsByToolInFirstAppearanceOrder() {
        let tools = [tool("1", "read_file"), tool("2", "edit_file"), tool("3", "read_file"), tool("4", "read_file"),
                     tool("5", "edit_file")]
        XCTAssertEqual(RunSummary.summary(tools), "Read 3 files, edited 2 files")
        XCTAssertEqual(RunSummary.summary([tool("1", "web_search"), tool("2", "fetch_url")]),
                       "Searched the web 1 time, fetched 1 page")
        XCTAssertEqual(RunSummary.summary([tool("1", "list_files"), tool("2", "mystery"), tool("3", "use_skill")]),
                       "Listed 1 folder, used 1 tool, used 1 skill")
    }

    func testRowsFoldRunsButNeverTheApproval() {
        let user = AgentItem(id: "u", kind: .user, text: "go")
        let answer = AgentItem(id: "a", kind: .assistant, text: "done")
        let rows = RunSummary.rows([user, tool("1", "read_file"), tool("2", "read_file"), tool("3", "edit_file", "waiting"),
                                    tool("4", "read_file"), answer])
        XCTAssertEqual(rows.count, 5)
        XCTAssertEqual(rows[0], .item(user))
        guard case let .group(_, summary, items) = rows[1] else { return XCTFail("expected a group") }
        XCTAssertEqual(summary, "Read 2 files")
        XCTAssertEqual(items.count, 2)
        XCTAssertEqual(rows[2].id, "3", "the waiting tool stands alone")
        XCTAssertEqual(rows[3].id, "4", "a single tool call is not folded")
        XCTAssertEqual(rows[4], .item(answer))
    }

    // MARK: the schedule in words

    func testScheduleDescriptions() {
        XCTAssertEqual(ScheduleText.describe(TaskSchedule(kind: .once, at: "2026-10-27T14:00")), "Once, 27 Oct at 14:00")
        XCTAssertEqual(ScheduleText.describe(TaskSchedule(kind: .interval, everyMinutes: 30)), "Every 30 minutes")
        XCTAssertEqual(ScheduleText.describe(TaskSchedule(kind: .interval, everyMinutes: 60)), "Every hour")
        XCTAssertEqual(ScheduleText.describe(TaskSchedule(kind: .interval, everyMinutes: 120)), "Every 2 hours")
        XCTAssertEqual(ScheduleText.describe(TaskSchedule(kind: .daily, at: "9:00")), "Every day at 09:00")
        XCTAssertEqual(ScheduleText.describe(TaskSchedule(kind: .weekly, at: "09:00", weekdays: [3, 1])), "Every Mon, Wed at 09:00")
        XCTAssertEqual(ScheduleText.describe(TaskSchedule(kind: .weekly, at: "07:15", weekdays: [1, 2, 3, 4, 5])),
                       "Every weekday at 07:15")
        XCTAssertEqual(ScheduleText.describe(TaskSchedule(kind: .weekly, at: "10:00", weekdays: [0, 6])), "Every weekend at 10:00")
        XCTAssertEqual(ScheduleText.describe(TaskSchedule(kind: .weekly, at: "10:00", weekdays: [0, 6, 5])), "Every Fri, Sat, Sun at 10:00")
        XCTAssertEqual(ScheduleText.describe(TaskSchedule(kind: .once, at: "garbage")), "Once")
    }

    // MARK: the migration

    private final class FakeServer: ConversationImporting, @unchecked Sendable {
        var keepItems = true
        var posted: [JSON] = []
        var deleted: [String] = []

        func importConversation(_ body: JSON) async throws -> JSON {
            posted.append(body)
            var answer: [String: JSON] = ["id": .string("srv\(posted.count)"), "title": body["title"]]
            if keepItems { answer["items"] = body["items"] }
            return .object(answer)
        }

        func deleteConversation(id: String) async throws { deleted.append(id) }
    }

    private func storeWithOne() throws -> (ConversationStore, URL) {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let store = ConversationStore(directory: dir)
        var c = Conversation(messages: [ChatMessage(role: .user, content: "Which?\nA) x\nB) y")])
        c.messages.append(ChatMessage(role: .assistant, content: "", choices: [ChoiceScore(label: "A", text: "x", probability: 0.75),
                                                                                 ChoiceScore(label: "B", text: "y", probability: 0.25)], best: 0))
        c.messages.append(ChatMessage(role: .user, content: "thanks"))
        c.messages.append(ChatMessage(role: .assistant, content: "welcome", reasoning: "r"))
        try store.save(c)
        try store.save(Conversation()) // an empty one: nothing to move
        return (store, dir)
    }

    func testMigrationMovesAndDeletesTheLocalCopy() async throws {
        let (store, dir) = try storeWithOne()
        defer { try? FileManager.default.removeItem(at: dir) }
        let server = FakeServer()
        let out = await ConversationMigration.run(store: store, client: server)
        XCTAssertEqual(out, .init(moved: 1, kept: 0))
        XCTAssertTrue(store.list().isEmpty)
        XCTAssertEqual(server.posted.count, 1)
        let items = server.posted[0]["items"].array ?? []
        XCTAssertEqual(items.map { $0["kind"].string ?? "" }, ["user", "assistant", "user", "assistant"])
        XCTAssertEqual(items[1]["text"].string, "A) x: 75%\nB) y: 25%\nPicked A) x")
        XCTAssertEqual(items[3]["text"].string, "welcome")
        XCTAssertEqual(server.posted[0]["title"].string, "Which?")
    }

    func testMigrationKeepsTheLocalCopyWhenTheServerDroppedTheItems() async throws {
        let (store, dir) = try storeWithOne()
        defer { try? FileManager.default.removeItem(at: dir) }
        let server = FakeServer()
        server.keepItems = false
        let out = await ConversationMigration.run(store: store, client: server)
        XCTAssertEqual(out, .init(moved: 0, kept: 1))
        XCTAssertEqual(store.list().count, 1, "nothing is lost on a server that ignores items")
        XCTAssertEqual(server.deleted, ["srv1"], "the empty conversation it made is removed again")
    }

    // MARK: the preview

    func testPreviewKinds() {
        XCTAssertEqual(PreviewFile(data: Data("# x".utf8), contentType: "text/markdown", path: "a").kind, .markdown)
        XCTAssertEqual(PreviewFile(data: Data(), contentType: "text/html; charset=utf-8", path: "a").kind, .html)
        XCTAssertEqual(PreviewFile(data: Data(), contentType: "image/png", path: "a.png").kind, .image)
        XCTAssertEqual(PreviewFile(data: Data("int x;".utf8), contentType: "application/octet-stream", path: "a.c").kind, .text)
        XCTAssertEqual(PreviewFile(data: Data([0, 1, 2]), contentType: "application/octet-stream", path: "a.bin").kind, .binary)
        XCTAssertEqual(PreviewFile(data: Data(), contentType: "", path: "README.md").kind, .markdown)
    }
}
