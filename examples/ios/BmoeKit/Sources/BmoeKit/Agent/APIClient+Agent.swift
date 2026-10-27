import Foundation

/// A workspace file's bytes for the preview, with the type the server said they are.
public struct PreviewFile: Equatable, Sendable {
    public let data: Data
    public let contentType: String
    public let path: String

    public init(data: Data, contentType: String, path: String) {
        self.data = data
        self.contentType = contentType
        self.path = path
    }

    public enum Kind: Equatable, Sendable { case markdown, html, image, text, binary }

    /// What the preview draws, from the content type first and the name second.
    public var kind: Kind {
        let type = contentType.lowercased()
        let ext = (path as NSString).pathExtension.lowercased()
        if type.hasPrefix("text/markdown") || ["md", "markdown"].contains(ext) { return .markdown }
        if type.hasPrefix("text/html") || ["html", "htm"].contains(ext) { return .html }
        if type.hasPrefix("image/") { return .image }
        if type.hasPrefix("text/") || type.contains("json") || type.contains("xml") || type.contains("javascript") { return .text }
        // Unknown type: text when it decodes as UTF-8 and has no NUL, which is what source files are.
        if !data.contains(0), String(data: data, encoding: .utf8) != nil { return .text }
        return .binary
    }

    public var text: String { String(decoding: data, as: UTF8.self) }
}

/// The agent's routes (docs/agent.md, "Routes"). Every call is a short exchange with the server:
/// a run goes on in the server after `messages` answers, and reports on the event stream.
extension APIClient: ConversationImporting {
    static func escape(_ s: String) -> String { s.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? s }

    // MARK: workspaces

    public func workspaces() async throws -> [Workspace] { Workspace.list(try await get("/api/workspaces", timeout: Self.quick)) }

    public func addWorkspace(path: String, name: String?) async throws -> Workspace {
        var body: [String: JSON] = ["path": .string(path)]
        if let name, !name.isEmpty { body["name"] = .string(name) }
        return Workspace(try await send("POST", "/api/workspaces", .object(body), timeout: Self.quick))
    }

    public func removeWorkspace(id: String) async throws {
        _ = try await send("DELETE", "/api/workspaces/" + Self.escape(id), nil, timeout: Self.quick)
    }

    // MARK: conversations

    public func conversations() async throws -> [ConversationSummary] {
        ConversationSummary.list(try await get("/api/conversations", timeout: Self.quick))
    }

    public func createConversation(workspace: String?, mode: AgentMode?, route: String?, title: String? = nil) async throws
        -> AgentConversation
    {
        var body: [String: JSON] = [:]
        if let workspace, !workspace.isEmpty { body["workspace"] = .string(workspace) }
        if let mode { body["mode"] = .string(mode.rawValue) }
        if let route, !route.isEmpty { body["route"] = .string(route) }
        if let title, !title.isEmpty { body["title"] = .string(title) }
        return AgentConversation(try await send("POST", "/api/conversations", .object(body), timeout: Self.quick))
    }

    public func importConversation(_ body: JSON) async throws -> JSON {
        try await send("POST", "/api/conversations", body, timeout: Self.quick)
    }

    public func conversation(id: String) async throws -> AgentConversation {
        AgentConversation(try await get("/api/conversations/" + Self.escape(id), timeout: Self.quick))
    }

    /// Changes what is given: a nil field is left as it is; an empty workspace is none.
    public func updateConversation(id: String, title: String? = nil, mode: AgentMode? = nil, route: String? = nil,
                                   workspace: String? = nil) async throws
    {
        var body: [String: JSON] = [:]
        if let title { body["title"] = .string(title) }
        if let mode { body["mode"] = .string(mode.rawValue) }
        if let route { body["route"] = .string(route) }
        if let workspace { body["workspace"] = workspace.isEmpty ? .null : .string(workspace) }
        _ = try await send("PATCH", "/api/conversations/" + Self.escape(id), .object(body), timeout: Self.quick)
    }

    public func deleteConversation(id: String) async throws {
        _ = try await send("DELETE", "/api/conversations/" + Self.escape(id), nil, timeout: Self.quick)
    }

    /// Sends a message; answers the person's item at once (`202`), the run reports on the stream.
    @discardableResult
    public func sendMessage(conversation id: String, text: String, think: Bool? = nil) async throws -> AgentItem {
        var body: [String: JSON] = ["text": .string(text)]
        if let think { body["think"] = .bool(think) }
        return AgentItem(try await send("POST", "/api/conversations/\(Self.escape(id))/messages", .object(body), timeout: Self.quick)["item"])
    }

    /// Drops `item` and everything after it, then sends `text` in its place.
    public func editMessage(conversation id: String, item: String, text: String) async throws {
        _ = try await send("POST", "/api/conversations/\(Self.escape(id))/edit", ["item": .string(item), "text": .string(text)],
                           timeout: Self.quick)
    }

    public func answerApproval(conversation id: String, item: String, approve: Bool) async throws {
        _ = try await send("POST", "/api/conversations/\(Self.escape(id))/approval",
                           ["item": .string(item), "decision": .string(approve ? "approve" : "deny")], timeout: Self.quick)
    }

    /// Stops the run and its helpers; answers once it has stopped, which can take a moment.
    public func stopConversation(id: String) async throws {
        _ = try await send("POST", "/api/conversations/\(Self.escape(id))/stop", nil, timeout: Self.slow)
    }

    /// A workspace file the agent produced or changed, for the preview.
    public func file(conversation id: String, path: String) async throws -> PreviewFile {
        let p = path.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed.subtracting(CharacterSet(charactersIn: "&=+?#")))
            ?? path
        let (data, response) = try await session.data(
            for: request("GET", "/api/conversations/\(Self.escape(id))/file?path=\(p)", body: nil, timeout: Self.quick))
        let http = response as? HTTPURLResponse
        let status = http?.statusCode ?? 0
        guard (200..<300).contains(status) else { throw APIError(status: status, body: data) }
        return PreviewFile(data: data, contentType: http?.value(forHTTPHeaderField: "Content-Type") ?? "", path: path)
    }

    // MARK: skills

    public func skills() async throws -> [Skill] { Skill.list(try await get("/api/skills", timeout: Self.quick)) }

    /// Writes a person's skill: a new one, or `existing` replaced.
    public func saveSkill(name: String, description: String, body: String, existing: String?) async throws {
        let json: JSON = ["name": .string(name), "description": .string(description), "body": .string(body)]
        if let existing {
            _ = try await send("PUT", "/api/skills/" + Self.escape(existing), json, timeout: Self.quick)
        } else {
            _ = try await send("POST", "/api/skills", json, timeout: Self.quick)
        }
    }

    public func setSkill(name: String, enabled: Bool) async throws {
        _ = try await send("PATCH", "/api/skills/" + Self.escape(name), ["enabled": .bool(enabled)], timeout: Self.quick)
    }

    public func deleteSkill(name: String) async throws {
        _ = try await send("DELETE", "/api/skills/" + Self.escape(name), nil, timeout: Self.quick)
    }

    // MARK: tasks

    public func tasks() async throws -> [AgentTask] { AgentTask.list(try await get("/api/tasks", timeout: Self.quick)) }

    @discardableResult
    public func saveTask(_ t: AgentTask) async throws -> AgentTask {
        if t.id.isEmpty { return AgentTask(try await send("POST", "/api/tasks", t.body, timeout: Self.quick)) }
        return AgentTask(try await send("PUT", "/api/tasks/" + Self.escape(t.id), t.body, timeout: Self.quick))
    }

    public func deleteTask(id: String) async throws {
        _ = try await send("DELETE", "/api/tasks/" + Self.escape(id), nil, timeout: Self.quick)
    }

    public func runTask(id: String) async throws {
        _ = try await send("POST", "/api/tasks/\(Self.escape(id))/run", nil, timeout: Self.quick)
    }

    // MARK: providers

    public func providers() async throws -> [Provider] { Provider.list(try await get("/api/providers", timeout: Self.quick)) }

    /// Saves a provider; the key goes only when one was typed: an empty field keeps the saved one.
    public func saveProvider(_ p: Provider, apiKey: String) async throws {
        var body: [String: JSON] = ["name": .string(p.name), "base_url": .string(p.baseURL), "models": .array(p.models.map(JSON.string))]
        if !apiKey.isEmpty { body["api_key"] = .string(apiKey) }
        if p.id.isEmpty {
            _ = try await send("POST", "/api/providers", .object(body), timeout: Self.quick)
        } else {
            _ = try await send("PUT", "/api/providers/" + Self.escape(p.id), .object(body), timeout: Self.quick)
        }
    }

    public func deleteProvider(id: String) async throws {
        _ = try await send("DELETE", "/api/providers/" + Self.escape(id), nil, timeout: Self.quick)
    }
}
