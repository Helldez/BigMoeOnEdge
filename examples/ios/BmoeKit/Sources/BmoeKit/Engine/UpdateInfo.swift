import Foundation

/// What `GET /api/update` answers (docs/server-api.md): this engine's version, the latest
/// release's, whether it is newer, and the page that says what changed. The feed carries no
/// installer for a phone, so the app sends the person to `notes`; this app is built from source.
public struct UpdateInfo: Equatable, Sendable {
    public let current: String
    public let latest: String
    public let available: Bool
    public let notes: String

    /// The page to open: `notes` when it is https (the server checks the same), else none.
    public var releasePage: URL? {
        guard notes.hasPrefix("https://") else { return nil }
        return URL(string: notes)
    }

    public init(_ j: JSON) {
        current = j["current"].string(or: "")
        latest = j["latest"].string(or: "")
        available = j["available"].bool ?? false
        notes = j["notes"].string(or: "")
    }
}
