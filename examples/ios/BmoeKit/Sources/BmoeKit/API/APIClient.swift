import Foundation

/// A non-2xx answer from the server: the message its JSON body carried, for a person, and its
/// `code`, for the app to decide on (docs/server-api.md). Empty when the body had none.
public struct APIError: LocalizedError, Equatable, Sendable {
    public let status: Int
    public let message: String
    public let code: String

    public init(status: Int, message: String, code: String = "") {
        self.status = status
        self.message = message
        self.code = code
    }

    public var errorDescription: String? { message }

    /// A full context: the fix is a new chat, and the screen says so.
    public static let contextFull = "context_full"
    /// A turn refused with no model loaded: the fix is on Models.
    public static let noModel = "no_model"
    /// The text was not a repository id or URL: the screen searches for it instead.
    public static let notARepo = "not_a_repo"

    /// The server's `{"error", "code"}` when it sent them, otherwise the bare status.
    public init(status: Int, body: Data) {
        let json = try? JSON(parsing: body)
        let text = String(decoding: body, as: UTF8.self)
        var message = json?["error"].string ?? ""
        if message.isEmpty {
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            message = !trimmed.isEmpty && trimmed.count < 300 ? trimmed : "HTTP \(status)"
        }
        self.init(status: status, message: message, code: json?["code"].string ?? "")
    }
}

/// The client of bmoe-server's HTTP API (docs/server-api.md), the one API every UI of this project
/// speaks. The server runs inside this app, on loopback, started with a key only the app holds:
/// every request carries it, since on a phone any installed app can reach loopback.
///
/// Every call has a timeout fitted to what it waits for, so a server that stopped answering is an
/// error and not a screen that hangs. A chat stream and a decision wait their turn behind a
/// generation and then a prefill of minutes, so they wait as long as it takes; the event stream
/// has a watchdog instead, since the server writes to it at least every `pingInterval`.
public final class APIClient: @unchecked Sendable {
    public let baseURL: URL
    public let apiKey: String
    private let session: URLSession

    /// A read the server answers from memory: the config, the lists, a cancel.
    static let quick: TimeInterval = 15
    /// A load, an unload, a plan that measures the phone: tens of seconds, minutes at worst.
    static let slow: TimeInterval = 10 * 60
    /// Wait as long as it takes: a chat stream and a decision wait their turn, then prefill.
    static let unbounded: TimeInterval = 7 * 24 * 3600
    /// How often the server writes a keep-alive to the event stream (docs/server-api.md).
    public static let pingInterval: TimeInterval = 15
    /// Two missed pings and a margin: the event stream is dead and is reconnected.
    static let eventsIdle: TimeInterval = 2 * pingInterval + 10

    public init(baseURL: URL, apiKey: String) {
        self.baseURL = baseURL
        self.apiKey = apiKey
        let config = URLSessionConfiguration.ephemeral
        // The event stream and a chat hold a connection each for as long as they run, beside the
        // calls the screens make meanwhile.
        config.httpMaximumConnectionsPerHost = 16
        config.timeoutIntervalForResource = Self.unbounded
        config.requestCachePolicy = .reloadIgnoringLocalCacheData
        session = URLSession(configuration: config)
    }

    /// A key nobody can guess: 32 random bytes, hex. What the app starts the server with.
    public static func randomKey() -> String {
        var g = SystemRandomNumberGenerator()
        return (0..<32).map { _ in String(format: "%02x", UInt8.random(in: 0...255, using: &g)) }.joined()
    }

    // MARK: server and session

    public func info() async throws -> ServerInfo { ServerInfo(try await get("/api/info", timeout: Self.quick)) }
    public func params() async throws -> [ParamSpec] { ParamSpec.parse(try await get("/api/params", timeout: Self.quick)) }
    public func config() async throws -> ConfigState { ConfigState(try await get("/api/config", timeout: Self.quick)) }

    /// Sets `values` in the person's layer and resets `reset` to the server's defaults.
    public func updateConfig(values: [String: JSON] = [:], reset: [String] = [], autoPlan: Bool? = nil) async throws
        -> ConfigState
    {
        var body: [String: JSON] = ["values": .object(values), "reset": .array(reset.map(JSON.string))]
        if let autoPlan { body["auto_plan"] = .bool(autoPlan) }
        return ConfigState(try await send("PUT", "/api/config", .object(body), timeout: Self.quick))
    }

    // A load answers once it is under way, but first unloads the model and, in auto mode, measures
    // the phone; an unload waits out a load in progress. Minutes, not seconds, on a large model.
    public func load(model: String? = nil) async throws {
        _ = try await send("POST", "/api/session/load", model.map { ["model": .string($0)] }, timeout: Self.slow)
    }

    public func unload() async throws { _ = try await send("POST", "/api/session/unload", nil, timeout: Self.slow) }
    public func cancel() async throws { _ = try await send("POST", "/api/cancel", nil, timeout: Self.quick) }

    /// A decision (docs/decide.md): the model picks one of the choices from a single prefill.
    public func decide(_ body: JSON) async throws -> JSON {
        try await send("POST", "/api/decide", body, timeout: Self.unbounded)
    }

    // MARK: models and the plan

    public func models() async throws -> ModelsList { ModelsList(try await get("/api/models", timeout: Self.quick)) }

    public func hfSearch(query: String, token: String) async throws -> HFSearch {
        HFSearch(try await send("POST", "/api/hf/search", HF.searchBody(query: query, token: token), timeout: Self.slow))
    }

    public func hfModels(repo: String, token: String) async throws -> HFModels {
        HFModels(try await send("POST", "/api/hf/models", HF.body(repo: repo, token: token), timeout: Self.slow))
    }

    public func hfInspect(repo: String, name: String, token: String) async throws -> HFFacts {
        HFFacts(try await send("POST", "/api/hf/inspect", HF.body(repo: repo, name: name, token: token), timeout: Self.slow))
    }

    /// Adds the model to the catalog; answers the new entry's id.
    public func hfAdd(repo: String, name: String, token: String) async throws -> String {
        try await send("POST", "/api/hf/add", HF.body(repo: repo, name: name, token: token), timeout: Self.slow)["id"]
            .string(or: "")
    }

    public func removeAdded(id: String) async throws {
        let escaped = id.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? id
        _ = try await send("DELETE", "/api/hf/entry/" + escaped, nil, timeout: Self.quick)
    }

    /// Deletes the partial files a stopped download of the entry left in the models directory, by
    /// catalog id: the server knows which files the entry names, and deletes only those.
    public func deletePartial(id: String) async throws {
        let escaped = id.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? id
        _ = try await send("DELETE", "/api/models/partial/" + escaped, nil, timeout: Self.quick)
    }

    /// A measurement, or (never measuring) the plan the loaded model runs with, else the last one held.
    public func plan(measure: Bool) async throws -> PlanResult {
        PlanResult(try await get(measure ? "/api/plan?measure=1" : "/api/plan?cached=1", timeout: Self.slow))
    }

    public func applyPlan() async throws -> ConfigState {
        ConfigState(try await send("POST", "/api/plan/apply", nil, timeout: Self.quick))
    }

    // MARK: whose server this is

    /// Whether the server on the port is the one this app started: it refuses a key it was not
    /// given and accepts the one it was. Another program's server answers the first (an open one)
    /// or refuses the second (one with its own key). Throws when nothing answers.
    public func isOurs() async throws -> Bool {
        let stranger = APIClient(baseURL: baseURL, apiKey: Self.randomKey())
        guard try await stranger.status("/api/info") == 401 else { return false }
        return try await status("/api/info") == 200
    }

    private func status(_ path: String) async throws -> Int {
        let (_, response) = try await session.data(for: request("GET", path, body: nil, timeout: Self.quick))
        return (response as? HTTPURLResponse)?.statusCode ?? 0
    }

    // MARK: streams

    /// `GET /api/events` for as long as the server keeps it open. Cancelling the task that reads it
    /// closes the connection; two missed pings end it with a timeout, and the caller reconnects.
    public func events() -> AsyncThrowingStream<SSEEvent, Error> {
        stream(request("GET", "/api/events", body: nil, timeout: Self.eventsIdle))
    }

    /// `POST /v1/chat/completions` with `stream: true`: every `data:` payload, `[DONE]` included.
    /// A 4xx/5xx is an `APIError` before anything streams. Cancelling the reading task closes the
    /// connection, which is how a client gives its place in the turn line up.
    public func chat(_ body: JSON) -> AsyncThrowingStream<String, Error> {
        let events = stream(request("POST", "/v1/chat/completions", body: body, timeout: Self.unbounded))
        return AsyncThrowingStream { continuation in
            let task = Task {
                do {
                    for try await e in events { continuation.yield(e.data) }
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    // MARK: plumbing

    private func request(_ method: String, _ path: String, body: JSON?, timeout: TimeInterval) -> URLRequest {
        var r = URLRequest(url: URL(string: path, relativeTo: baseURL)!.absoluteURL)
        r.httpMethod = method
        r.timeoutInterval = timeout
        r.setValue("application/json, text/event-stream", forHTTPHeaderField: "Accept")
        r.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
        // Every write says it is JSON, body or no body: the server refuses one that does not.
        if method != "GET" {
            r.setValue("application/json", forHTTPHeaderField: "Content-Type")
            r.httpBody = body?.data() ?? Data()
        }
        return r
    }

    private func get(_ path: String, timeout: TimeInterval) async throws -> JSON {
        try await exchange(request("GET", path, body: nil, timeout: timeout))
    }

    private func send(_ method: String, _ path: String, _ body: JSON?, timeout: TimeInterval) async throws -> JSON {
        try await exchange(request(method, path, body: body, timeout: timeout))
    }

    private func exchange(_ r: URLRequest) async throws -> JSON {
        let (data, response) = try await session.data(for: r)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(status) else { throw APIError(status: status, body: data) }
        if data.allSatisfy({ $0 == 0x20 || $0 == 0x0A || $0 == 0x0D || $0 == 0x09 }) { return .object([:]) }
        return try JSON(parsing: data)
    }

    private func stream(_ r: URLRequest) -> AsyncThrowingStream<SSEEvent, Error> {
        AsyncThrowingStream { continuation in
            let task = Task {
                do {
                    let (bytes, response) = try await session.bytes(for: r)
                    let status = (response as? HTTPURLResponse)?.statusCode ?? 0
                    guard (200..<300).contains(status) else {
                        var body = Data()
                        for try await b in bytes { body.append(b) }
                        throw APIError(status: status, body: body)
                    }
                    var parser = SSEParser()
                    // Decoded a line at a time: a line break is ASCII, so it never splits a
                    // character, while a network chunk can.
                    var pending = Data()
                    for try await b in bytes {
                        pending.append(b)
                        if b == 0x0A || b == 0x0D {
                            for e in parser.push(String(decoding: pending, as: UTF8.self)) { continuation.yield(e) }
                            pending.removeAll(keepingCapacity: true)
                        }
                    }
                    for e in parser.push(String(decoding: pending, as: UTF8.self)) + parser.end() {
                        continuation.yield(e)
                    }
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }
}
