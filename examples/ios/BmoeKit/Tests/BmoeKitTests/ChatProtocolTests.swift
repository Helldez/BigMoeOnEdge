import XCTest
@testable import BmoeKit

/// The chat-completions protocol as the app speaks it: what goes out, what each chunk means.
final class ChatProtocolTests: XCTestCase {
    func testTheWholeConversationGoesOutWithReasoningOnAssistantTurns() {
        let body = ChatProtocol.requestBody([
            ChatMessage(role: .user, content: "hi"),
            ChatMessage(role: .assistant, content: "hello", reasoning: "greet back", metrics: "1 tok/s"),
            ChatMessage(role: .user, content: "and?"),
        ])
        let msgs = body["messages"].array ?? []
        XCTAssertEqual(msgs.count, 3)
        XCTAssertEqual(msgs[0]["content"].string, "hi")
        XCTAssertEqual(msgs[1]["reasoning_content"].string, "greet back")
        XCTAssertTrue(msgs[0]["reasoning_content"].isNull)
        XCTAssertTrue(msgs[1]["metrics"].isNull)
        XCTAssertEqual(body["stream"].bool, true)
    }

    func testADecisionAndItsQuestionStayOut() {
        let body = ChatProtocol.requestBody([
            ChatMessage(role: .user, content: "pick"),
            ChatMessage(role: .assistant, content: "", choices: [ChoiceScore(label: "A", text: "x", probability: 0.9)], best: 0),
            ChatMessage(role: .user, content: "next"),
        ], maxTokens: 64, think: false)
        XCTAssertEqual(body["messages"].array?.count, 1)
        XCTAssertEqual(body["messages"][0]["content"].string, "next")
        XCTAssertEqual(body["max_tokens"], .int(64))
        XCTAssertEqual(body["think"], .bool(false))
    }

    func testDeltasResetsAndTheLastChunk() throws {
        let c1 = try XCTUnwrap(ChatProtocol.parseChunk(#"{"choices":[{"delta":{"role":"assistant","content":"Hel"},"finish_reason":null}]}"#))
        XCTAssertEqual(c1.content, "Hel")
        XCTAssertNil(c1.reasoning)
        XCTAssertFalse(c1.reset)
        XCTAssertNil(c1.finish)

        let c2 = try XCTUnwrap(ChatProtocol.parseChunk(
            #"{"choices":[{"delta":{"content":"Hello","reasoning_content":"thinking"},"finish_reason":null}],"bmoe":{"reset":true}}"#))
        XCTAssertTrue(c2.reset)
        XCTAssertEqual(c2.content, "Hello")
        XCTAssertEqual(c2.reasoning, "thinking")

        let c3 = try XCTUnwrap(ChatProtocol.parseChunk(
            #"{"choices":[{"delta":{},"finish_reason":"stop"}],"bmoe":{"summary":{"tokens":3,"tok_s":2.5}}}"#))
        XCTAssertEqual(c3.finish, "stop")
        XCTAssertEqual(c3.summary?["tokens"].int, 3)
    }

    func testDoneErrorsAndUsageOnlyChunks() throws {
        XCTAssertNil(ChatProtocol.parseChunk("[DONE]"))
        XCTAssertNil(ChatProtocol.parseChunk(""))
        let full = try XCTUnwrap(ChatProtocol.parseChunk(#"{"error":{"message":"any wording","code":"context_full"}}"#))
        XCTAssertEqual(full.error, TurnError(message: "any wording", code: APIError.contextFull))
        XCTAssertEqual(ChatProtocol.parseChunk(#"{"error":{"message":"no code"}}"#)?.error?.code, "")
        let usage = try XCTUnwrap(ChatProtocol.parseChunk(#"{"choices":[],"usage":{"prompt_tokens":1}}"#))
        XCTAssertNil(usage.content)
        XCTAssertNil(usage.finish)
    }

    func testTurnMetricsReadsTheSummary() throws {
        var s = try JSON(parsing: #"""
            {"tok_s":3.14159,"tokens":42,"prefill_s":1.26,"prefill_tps":13.6,"n_prompt":17,"n_past":59,"cache_hit_pct":81.4,
             "mtp_decodes":0,"mtp_drafted":0,"loop_overhead_s_tok":0.0}
            """#)
        XCTAssertEqual(ChatProtocol.turnMetrics(s, nCtx: 4096, cancelled: false),
                       "decode 3.1 tok/s (42 tok) · prefill 13.6 tok/s (17 tok, 1.3 s) · ctx 59/4096 · cache 81%")
        if case var .object(o) = s {
            o["prefill_tps"] = nil
            s = .object(o)
        }
        XCTAssertTrue(ChatProtocol.turnMetrics(s, nCtx: 0, cancelled: false).contains("prefill 13.5 tok/s (17 tok, 1.3 s)"))
        XCTAssertTrue(ChatProtocol.turnMetrics(s, nCtx: 0, cancelled: true).hasSuffix("· stopped"))
    }

    func testAPIErrorReadsTheBody() {
        let e = APIError(status: 400, body: Data(#"{"error":"x","code":"context_full"}"#.utf8))
        XCTAssertEqual(e.message, "x")
        XCTAssertEqual(e.code, APIError.contextFull)
        XCTAssertEqual(APIError(status: 502, body: Data()).message, "HTTP 502")
        XCTAssertEqual(APIError(status: 500, body: Data("not json".utf8)).code, "")
    }
}
