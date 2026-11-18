import Foundation

/// `POST /api/models/inspect`: the server's check of a catalog entry against this device, before
/// its download (docs/server-api.md, "Models"), as a first run holds it: asked when a model is
/// picked, kept for the visit, shown as the server worded it. No threshold and no wording of the
/// verdict lives here: the sentence and its tone are the server's. The web UI's
/// `ui/src/lib/modelCheck.ts`.
public struct ModelCheck: Equatable, Sendable {
    /// False when the storage cannot take the download, or the file is not a model.
    public let downloadable: Bool
    /// The answer in one sentence with no engine term; "" from an older server.
    public let summary: String
    /// The tone it is said in: `ok`, `warn`, `err`, or "" where nothing is claimed.
    public let tone: String
    /// The engine's own line and its two sentences, for whoever asks how it was checked.
    public let headline: String
    public let disk: String
    public let memory: String
    public let mode: String

    public init(_ j: JSON) {
        let v = j["verdict"]
        downloadable = v["downloadable"].bool(or: false)
        summary = v["summary"].string(or: "")
        tone = v["tone"].string(or: "")
        headline = v["headline"].string(or: "")
        disk = v["disk"].string(or: "")
        memory = v["memory"].string(or: "")
        mode = v["mode"].string(or: "")
    }

    /// The sentence to show: the plain one, or the headline of a server that has none.
    public var sentence: String { !summary.isEmpty ? summary : !headline.isEmpty ? headline : mode }
}

/// Where the check of one entry stands.
public enum CheckState: Equatable, Sendable {
    case pending
    case done(ModelCheck)
    case failed(String)

    /// The tone the server said its sentence in; none where it claimed none, or while it is not known.
    public var tone: FitTone {
        if case let .done(c) = self { return FitTone(tone: c.tone) }
        return .none
    }
}

extension Optional where Wrapped == CheckState {
    /// Whether the download may start: not while the check runs (its answer is a second away), and
    /// not when the server said the model cannot be downloaded here. A check that could not be made
    /// refuses nothing: the download has its own checks.
    public var mayDownload: Bool {
        switch self {
        case .none, .some(.pending): return false
        case .some(.failed): return true
        case let .some(.done(c)): return c.downloadable
        }
    }
}

extension FitTone {
    /// From the tone the server names beside its plain sentence; one this app does not know is none.
    public init(tone: String) {
        switch tone {
        case "ok": self = .ok
        case "warn": self = .warn
        case "err": self = .err
        default: self = .none
        }
    }
}
