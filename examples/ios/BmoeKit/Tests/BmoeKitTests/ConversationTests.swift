import XCTest
@testable import BmoeKit

/// The chat's client side: the shapes of docs/agent.md as the server sends them, the transcript
/// the events build, Answer again, and the move of the conversations this app used to keep to the
/// server.
final class ConversationTests: XCTestCase {
    private func json(_ s: String) throws -> JSON { try JSON(parsing: s) }

    // MARK: decoding the contract

    func testDecodesConversationListAndTimes() throws {
        let list = ConversationSummary.list(try json(#"""
            [{"id":"c1","title":"Fix the build","state":"running","updated":1761580800},
             {"id":"c2","updated":"2026-10-27T10:00:00Z"},
             {"id":"c3","updated":1761580800000}]
            """#))
        XCTAssertEqual(list.count, 3)
        XCTAssertEqual(list[0].state, "running")
        XCTAssertTrue(list[0].busy)
        XCTAssertEqual(list[0].updated, Date(timeIntervalSince1970: 1_761_580_800))
        XCTAssertEqual(list[2].updated, Date(timeIntervalSince1970: 1_761_580_800), "milliseconds read as such")
        XCTAssertNotNil(list[1].updated)
        // Missing fields read as their defaults.
        XCTAssertEqual(list[1].state, "idle")
        XCTAssertFalse(list[1].busy)
        XCTAssertEqual(list[1].displayTitle, "New chat")
    }

    func testDecodesEveryItemKind() throws {
        let c = ServerConversation(try json(#"""
            {"id":"c","title":"t","items":[
              {"id":"1","kind":"user","text":"hi"},
              {"id":"2","kind":"assistant","text":"**ok**","reasoning":"hmm","streaming":true},
              {"id":"5","kind":"notice","text":"Context nearly full","level":"warn"},
              {"id":"6","kind":"later"}]}
            """#))
        XCTAssertEqual(c.items.map(\.kind), [.user, .assistant, .notice, .other])
        XCTAssertEqual(c.items[1].reasoning, "hmm")
        XCTAssertTrue(c.items[1].streaming)
        XCTAssertEqual(c.items[2].level, "warn")
    }

    // MARK: the transcript

    func testItemUpsertsInPlaceAndAppendsInOrder() throws {
        var t = Transcript(conversation: "c")
        XCTAssertTrue(t.apply(event: "item", data: try json(#"{"conversation":"c","item":{"id":"1","kind":"user","text":"hi"}}"#)))
        t.apply(event: "item", data: try json(#"{"conversation":"c","item":{"id":"2","kind":"assistant","text":"","streaming":true}}"#))
        t.apply(event: "item", data: try json(#"{"conversation":"c","item":{"id":"3","kind":"notice","text":"n"}}"#))
        t.apply(event: "item", data: try json(#"{"conversation":"c","item":{"id":"2","kind":"assistant","text":"x"}}"#))
        XCTAssertEqual(t.items.map(\.id), ["1", "2", "3"], "a changed item stays where it first appeared")
        XCTAssertEqual(t.items[1].text, "x")
        XCTAssertFalse(t.items[1].streaming)
    }

    func testDeltaAppendsAndStartsAnUnknownItem() throws {
        var t = Transcript(conversation: "c", items: [ConversationItem(id: "a", kind: .assistant, text: "Hel", streaming: true)])
        t.apply(event: "delta", data: try json(#"{"conversation":"c","item":"a","text":"lo"}"#))
        t.apply(event: "delta", data: try json(#"{"conversation":"c","item":"a","reasoning":"think"}"#))
        XCTAssertEqual(t.items[0].text, "Hello")
        XCTAssertEqual(t.items[0].reasoning, "think")
        t.apply(event: "delta", data: try json(#"{"conversation":"c","item":"b","text":"new"}"#))
        XCTAssertEqual(t.items.last, ConversationItem(id: "b", kind: .assistant, text: "new", streaming: true))
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
        var t = Transcript(conversation: "c", items: ["1", "2", "3"].map { ConversationItem(id: $0, kind: .user) })
        t.truncate(from: "2")
        XCTAssertEqual(t.items.map(\.id), ["1"])
    }

    func testConversationEventsUpdateTheList() throws {
        var list = [ConversationSummary(id: "a", title: "old", state: "idle")]
        let row = ConversationList.apply(&list, event: try json(#"{"id":"a","title":"new","state":"running","updated":10}"#))
        XCTAssertEqual(row?.state, "running")
        XCTAssertEqual(list[0].title, "new")
        XCTAssertEqual(list[0].updated, Date(timeIntervalSince1970: 10))
        ConversationList.apply(&list, event: try json(#"{"id":"b","title":"from elsewhere","state":"running"}"#))
        XCTAssertEqual(list.map(\.id), ["b", "a"])
    }

    // MARK: Answer again

    func testAnswerAgainSitsOnTheLastWholeAnswerAndResendsTheLastQuestion() throws {
        var t = Transcript(conversation: "c", items: [
            ConversationItem(id: "1", kind: .user, text: "q"), ConversationItem(id: "2", kind: .assistant, text: "a"),
            ConversationItem(id: "3", kind: .user, text: "again?"), ConversationItem(id: "4", kind: .assistant, text: "b"),
            ConversationItem(id: "5", kind: .notice, text: "slow"),
        ])
        // A notice after the answer does not move it; only the last answer carries it.
        XCTAssertEqual(t.answerAgainOn, "4")
        XCTAssertEqual(t.lastUser?.id, "3")
        XCTAssertEqual(t.lastUser?.text, "again?")
        // The edit route resends the question as it was, and the screen drops it and what follows.
        XCTAssertEqual(Transcript.editBody(item: "3", text: "again?"), ["item": "3", "text": "again?"])
        t.truncate(from: "3")
        XCTAssertEqual(t.items.map(\.id), ["1", "2"])
    }

    func testNoAnswerAgainWhileStreamingOrWithoutAQuestion() {
        let user = ConversationItem(id: "1", kind: .user, text: "q")
        XCTAssertNil(Transcript(conversation: "c", items: [user, ConversationItem(id: "2", kind: .assistant, streaming: true)]).answerAgainOn)
        XCTAssertNil(Transcript(conversation: "c", items: [user, ConversationItem(id: "2", kind: .assistant, text: "a"),
                                                           ConversationItem(id: "3", kind: .user)]).answerAgainOn,
                     "a question waiting for its answer")
        XCTAssertNil(Transcript(conversation: "c", items: [ConversationItem(id: "1", kind: .assistant, text: "a")]).answerAgainOn)
        XCTAssertNil(Transcript(conversation: "c").answerAgainOn)
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
}
