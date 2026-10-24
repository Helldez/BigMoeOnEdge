import Foundation

/// A model the server found on this device (`GET /api/models`, `local`).
public struct LocalModel: Equatable, Sendable, Identifiable {
    public let name: String
    public let path: String
    /// The directory it was found in: the models directory, or one the app gave it to scan.
    public let dir: String
    /// The folder directly inside `dir` it sits in (a split set kept in a folder of its own), ""
    /// when it is in `dir` itself.
    public let folder: String
    public let bytes: Int64
    public let shards: Int
    /// Every file of the model that is present, in shard order: what a delete removes.
    public let files: [String]
    public let arch: String
    public let streamable: Bool
    public let complete: Bool
    /// What its header says it is for: `model`, `projector`, `draft`, `other`; "" when the header
    /// could not be read (`FileRole`).
    public let role: String
    public var id: String { path }

    public init(_ j: JSON) {
        name = j["name"].string(or: "")
        path = j["path"].string(or: "")
        dir = j["dir"].string(or: "")
        folder = j["folder"].string(or: "")
        bytes = j["bytes"].int64(or: 0)
        shards = j["shards"].int(or: 1)
        files = j["files"].strings
        arch = j["arch"].string(or: "")
        streamable = j["streamable"].bool(or: false)
        complete = j["complete"].bool(or: true)
        role = j["role"].string(or: "")
    }
}

/// What a gguf file is for, as the server reports it (`role`, docs/server-api.md, "Models"): a
/// model to chat with, or a companion file that only goes with one. The server decides, from the
/// header on this device and on inspect, from the name in a repository listing; this only names the
/// answer and keeps the companions apart so they do not read as models. The web UI and the Android
/// app do the same.
public enum FileRole {
    /// A file the server says is not a model to load. "" (an unreadable header) is not known to be one.
    public static func isCompanion(_ role: String) -> Bool { !role.isEmpty && role != "model" }

    public static func label(_ role: String) -> String {
        switch role {
        case "projector": return "projector"
        case "draft": return "draft head"
        case "model": return "model"
        default: return "not a model"
        }
    }

    /// The models first, the companion files after them, each in the order given.
    public static func split<T>(_ items: [T], role: (T) -> String) -> (models: [T], companions: [T]) {
        (items.filter { !isCompanion(role($0)) }, items.filter { isCompanion(role($0)) })
    }

    /// The date part of an ISO timestamp, "" when there is none.
    public static func isoDate(_ ts: String) -> String {
        ts.range(of: #"^\d{4}-\d{2}-\d{2}"#, options: .regularExpression) != nil ? String(ts.prefix(10)) : ""
    }
}

/// One file of a catalog entry, with the URL it is fetched from. Sizes are exact; 0 is a size the
/// host did not say (an entry made from a link whose `HEAD` answered none).
public struct CatalogFile: Equatable, Sendable, Codable, Hashable {
    public let name: String
    public let bytes: Int64
    public let url: String
    /// The digest a finished download must have, 64 lowercase hex digits; "" when the catalog names
    /// none, and the file is then taken as it came.
    public let sha256: String

    public init(name: String, bytes: Int64, url: String, sha256: String = "") {
        self.name = name
        self.bytes = bytes
        self.url = url
        self.sha256 = sha256
    }

    // A task in flight carries its entry's files (ModelDownloader's tag): one started by a build
    // from before the digest carries none, and still decodes.
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        bytes = try c.decode(Int64.self, forKey: .bytes)
        url = try c.decode(String.self, forKey: .url)
        sha256 = try c.decodeIfPresent(String.self, forKey: .sha256) ?? ""
    }

    /// 64 lowercase hex digits: a SHA-256 as the catalog spells it.
    public static func isSha256Hex(_ text: String) -> Bool {
        text.utf8.count == 64 && text.utf8.allSatisfy { (0x30...0x39).contains($0) || (0x61...0x66).contains($0) }
    }

    /// The app fetches a file itself, and only over https: the server allows plain http under an
    /// operator's mirror, which an app's transfers do not take.
    public static func isHTTPS(_ url: String) -> Bool {
        guard let u = URL(string: url), let host = u.host, !host.isEmpty else { return false }
        return u.scheme?.lowercased() == "https"
    }

    /// What the app writes, fetches and checks holds: a bare name, an https URL, a digest where
    /// there is one.
    public var isUsable: Bool {
        Self.isPlainName(name) && Self.isHTTPS(url) && (sha256.isEmpty || Self.isSha256Hex(sha256))
    }

    /// A catalog file name is a bare name: it is joined onto the models directory, so a separator,
    /// "." or ".." would name a file outside it. The server skips such a row as it loads the
    /// catalog; the app holds to the same rule rather than trust that it did.
    public static func isPlainName(_ name: String) -> Bool {
        !name.isEmpty && name != "." && name != ".." && !name.contains { "/\\:\0".contains($0) }
    }
}

/// One model the catalog offers, as `GET /api/models` lists it. The catalog is the server's data
/// file (`catalog/models.json`); on iOS the server has no `curl`, so the app fetches the files.
public struct CatalogEntry: Equatable, Sendable, Identifiable {
    public let id: String
    public let title: String
    public let quant: String
    public let blurb: String
    public let bytes: Int64
    /// The server's view of its own models directory: `on_disk`, `downloading`, `available`.
    public let serverStatus: String
    public let files: [CatalogFile]
    /// The first file's name: the one a model on the device is listed by.
    public var file: String { files[0].name }
    /// The catalog's line on the entry, where it has one.
    public let note: String
    /// Added from a repository or a link (`POST /api/hf/add`, `POST /api/models/url`): it can be
    /// forgotten; its files stay.
    public let added: Bool
    /// Bytes of it in the server's models directory, complete files and `.part` files alike, and
    /// whether a `.part` is there. Here the app downloads for itself and keeps what a stopped
    /// transfer owes as resume data instead (`ModelDownloader.partial`), so these describe only a
    /// `.part` that came from elsewhere.
    public let received: Int64
    public let partial: Bool

    public init?(_ j: JSON) {
        let files = (j["files"].array ?? []).map {
            CatalogFile(name: $0["name"].string(or: ""), bytes: $0["bytes"].int64(or: 0), url: $0["url"].string(or: ""),
                        sha256: $0["sha256"].string(or: "").lowercased())
        }
        // One file this app may not write, fetch or check drops the whole entry: half a split set
        // is no model.
        guard let id = j["id"].string, !files.isEmpty, files.allSatisfy(\.isUsable) else { return nil }
        self.id = id
        self.files = files
        title = j["title"].string(or: id)
        quant = j["quant"].string(or: "")
        blurb = j["blurb"].string(or: "")
        note = j["note"].string(or: "")
        bytes = j["bytes"].int64 ?? files.reduce(0) { $0 + $1.bytes }
        serverStatus = j["status"].string(or: "available")
        added = j["added"].bool(or: false)
        received = j["received"].int64(or: 0)
        partial = j["partial"].bool(or: false)
    }
}

public enum CatalogStatus: Sendable { case onDevice, downloading, available }

/// `GET /api/models`: the server's directories, what they hold, and the catalog.
public struct ModelsList: Equatable, Sendable {
    public let modelsDir: String
    public let scanDirs: [String]
    public let local: [LocalModel]
    public let catalog: [CatalogEntry]

    public init(_ j: JSON) {
        modelsDir = j["models_dir"].string(or: "")
        scanDirs = j["scan_dirs"].strings
        local = (j["local"].array ?? []).map(LocalModel.init)
        catalog = (j["catalog"].array ?? []).compactMap(CatalogEntry.init)
    }

    public static let empty = ModelsList(.object([:]))

    /// The name of every model file the server sees, shards included: what "on the device" means.
    public var fileNames: Set<String> {
        Set(local.flatMap { $0.files.map { ($0 as NSString).lastPathComponent } })
    }

    /// The on-device rule for a catalog entry. The server answers for its own models directory;
    /// the app adds what only it knows, the transfers in flight. A set is on the device only when
    /// every file is: the first shard alone lands seconds into a download of hours.
    public func status(of e: CatalogEntry, downloading: Set<String>) -> CatalogStatus {
        if downloading.contains(e.id) { return .downloading }
        let present = fileNames
        if e.serverStatus == "on_disk" || e.files.allSatisfy({ present.contains($0.name) }) { return .onDevice }
        return .available
    }
}

/// How the app writes a size: decimal units, the ones the model repositories quote. Quoting a model
/// as 17.0 GB and then refusing it for "15 GiB" reads as a bug though both are true. Below a gigabyte
/// the unit steps down, so a small file (a draft head, a test model) never reads as 0.0 GB.
public enum ByteText {
    public static func gb(_ bytes: Int64) -> String {
        let b = Double(max(0, bytes))
        if b >= 1e9 { return String(format: "%.1f GB", b / 1e9) }
        if b >= 1e6 { return String(format: "%.0f MB", b / 1e6) }
        return String(format: "%.0f KB", b / 1e3)
    }
    /// A catalog row's action when part of it is already on the device, worded as the web UI and
    /// the Android app word it, so the same stopped download reads the same everywhere.
    public static func resume(received: Int64, total: Int64) -> String {
        String(format: "Resume (%.1f of %@)", Double(received) / 1e9, gb(total))
    }
    public static func mib(_ mib: Double) -> String {
        mib >= 1024 ? String(format: "%.1f GiB", mib / 1024) : String(format: "%.0f MiB", mib)
    }
}
