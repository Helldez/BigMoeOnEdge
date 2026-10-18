import Foundation

/// A saved conversation. The conversation is the client's: the server keeps none, and every
/// request carries it whole.
public struct Conversation: Equatable, Sendable, Codable, Identifiable {
    public var id: UUID
    public var messages: [ChatMessage]
    /// When it was last answered.
    public var updated: Date
    /// The model it was last answered by, by file name.
    public var model: String

    public init(id: UUID = UUID(), messages: [ChatMessage] = [], updated: Date = Date(), model: String = "") {
        self.id = id
        self.messages = messages
        self.updated = updated
        self.model = model
    }

    /// The first line of the first message, cut to fit.
    public var title: String {
        let first = messages.first { $0.role == .user }?.content ?? ""
        let line = first.split(whereSeparator: \.isNewline).first.map(String.init) ?? ""
        let t = line.trimmingCharacters(in: .whitespaces)
        if t.isEmpty { return "New chat" }
        return t.count > Self.titleLength ? String(t.prefix(Self.titleLength - 1)) + "…" : t
    }

    static let titleLength = 60
}

/// The saved conversations: one JSON file each in a folder of the app's own, newest first. Written
/// whole on every change, through a temporary file, so a crash leaves the old one or the new one.
public final class ConversationStore: @unchecked Sendable {
    private let dir: URL
    private let lock = NSLock()

    public init(directory: URL) {
        dir = directory
    }

    public func list() -> [Conversation] {
        lock.lock()
        defer { lock.unlock() }
        let files = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
        let decoder = Self.decoder
        return files.filter { $0.pathExtension == "json" }
            .compactMap { try? decoder.decode(Conversation.self, from: Data(contentsOf: $0)) }
            .sorted { $0.updated > $1.updated }
    }

    public func save(_ c: Conversation) throws {
        lock.lock()
        defer { lock.unlock() }
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let data = try Self.encoder.encode(c)
        try data.write(to: file(c.id), options: .atomic)
    }

    public func delete(_ id: UUID) {
        lock.lock()
        defer { lock.unlock() }
        try? FileManager.default.removeItem(at: file(id))
    }

    private func file(_ id: UUID) -> URL { dir.appendingPathComponent(id.uuidString + ".json") }

    private static let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.dateEncodingStrategy = .iso8601
        return e
    }()

    private static let decoder: JSONDecoder = {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .iso8601
        return d
    }()
}
