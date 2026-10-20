import Foundation

/// One streamed chunk of `POST /v1/chat/completions`, reduced to what the screen applies.
/// `content` and `reasoning` are deltas to append, unless `reset` says they are the whole text so
/// far and replace what was shown (the server's chat parser reclassified text it had already sent,
/// a closing reasoning tag). `finish` is set on the last choice chunk, with the run `summary`.
public struct ChatChunk: Equatable, Sendable {
    public var content: String?
    public var reasoning: String?
    public var reset = false
    public var finish: String?
    public var summary: JSON?
    public var error: TurnError?
}

/// The OpenAI chat-completions protocol as this client speaks it: the request body for a
/// conversation, and each streamed chunk read back. JSON in, JSON out, no network, so it is tested
/// without a server. What the protocol means for a model is the server's business.
public enum ChatProtocol {
    public static let done = "[DONE]"

    /// The whole conversation, every time: the server reuses what its KV cache already holds. A
    /// decision is not a conversation turn, so a question with a chosen answer stays out, and so
    /// does a turn that failed with nothing said.
    public static func requestBody(_ messages: [ChatMessage], maxTokens: Int? = nil, think: Bool? = nil) -> JSON {
        var sent: [ChatMessage] = []
        for m in messages {
            if m.isDecision {
                if sent.last?.role == .user { sent.removeLast() }
                continue
            }
            if m.role == .assistant && m.content.isEmpty && m.reasoning.isEmpty { continue }
            sent.append(m)
        }
        let arr: [JSON] = sent.map { m in
            var o: [String: JSON] = ["role": .string(m.role.rawValue), "content": .string(m.content)]
            if m.role == .assistant && !m.reasoning.isEmpty { o["reasoning_content"] = .string(m.reasoning) }
            return .object(o)
        }
        var body: [String: JSON] = ["messages": .array(arr), "stream": .bool(true)]
        if let maxTokens { body["max_tokens"] = .int(Int64(maxTokens)) }
        if let think { body["think"] = .bool(think) }
        return .object(body)
    }

    /// One `data:` payload; nil for the `[DONE]` sentinel or anything that is not a chunk.
    public static func parseChunk(_ data: String) -> ChatChunk? {
        let t = data.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty, t != done, let o = try? JSON(parsing: t), o.object != nil else { return nil }
        if o["error"].object != nil {
            let message = o["error"]["message"].string(or: "")
            return ChatChunk(error: TurnError(message: message.isEmpty ? "the server reported an error" : message,
                                              code: o["error"]["code"].string(or: "")))
        }
        let bmoe = o["bmoe"]
        let choice = o["choices"][0]
        if choice.object == nil { return ChatChunk(summary: bmoe["summary"].object != nil ? bmoe["summary"] : nil) }
        let delta = choice["delta"]
        return ChatChunk(content: delta["content"].string, reasoning: delta["reasoning_content"].string,
                         reset: bmoe["reset"].bool(or: false), finish: choice["finish_reason"].string,
                         summary: bmoe["summary"].object != nil ? bmoe["summary"] : nil)
    }

    /// The figure under a finished answer, from the run summary: the decode rate and length, the
    /// prefill rate with the tokens it read and the time it took, the context used, the cache hit
    /// rate. Each rate names its phase; the same line the web UI and the Android app write.
    public static func turnMetrics(_ s: JSON, nCtx: Int, cancelled: Bool) -> String {
        let tokS = s["tok_s"].double(or: 0)
        let tokens = s["tokens"].int(or: 0)
        let prefill = s["prefill_s"].double(or: 0)
        let prefillTps = s["prefill_tps"].double(or: 0)
        let nPrompt = s["n_prompt"].int(or: -1)
        let nPast = s["n_past"].int(or: -1)
        let hit = s["cache_hit_pct"].double(or: -1)
        let decodes = s["mtp_decodes"].int64(or: 0)
        let drafted = s["mtp_drafted"].int64(or: 0)
        let accepted = s["mtp_accepted"].int64(or: 0)
        let overhead = s["loop_overhead_s_tok"].double(or: 0)
        var out = String(format: "decode %.1f tok/s (%d tok)", tokS, tokens)
        if prefill > 0 {
            // The tokens this turn prefilled: with the conversation's leading tokens reused from the
            // KV cache, that is the new message, not the whole history.
            let tps = prefillTps > 0 ? prefillTps : nPrompt > 0 ? Double(nPrompt) / prefill : 0
            out += String(format: " · prefill %.1f tok/s (", tps)
            if nPrompt >= 0 { out += "\(nPrompt) tok, " }
            out += String(format: "%.1f s)", prefill)
        }
        if nPast >= 0 && nCtx > 0 { out += " · ctx \(nPast)/\(nCtx)" }
        if hit >= 0 { out += String(format: " · cache %.0f%%", hit) }
        // With speculation on, the headline rate leaves out the drafting between decodes: show what
        // the turn really ran at, and how often the guesses were right.
        if decodes > 0 && drafted > 0 {
            if tokS > 0 { out += String(format: " · %.1f real", 1.0 / (1.0 / tokS + overhead)) }
            out += String(format: " · %.0f%% kept", 100.0 * Double(accepted) / Double(drafted))
        }
        if cancelled { out += " · stopped" }
        return out
    }
}
