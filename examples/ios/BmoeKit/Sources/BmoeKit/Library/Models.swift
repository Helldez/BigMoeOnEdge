import Foundation

/// A model the server found on this device (`GET /api/models`, `local`).
public struct LocalModel: Equatable, Sendable, Identifiable {
    public let name: String
    public let path: String
    /// The directory it was found in: the models directory, or one the app gave it to scan.
    public let dir: String
    public let bytes: Int64
    public let shards: Int
    /// Every file of the model that is present, in shard order: what a delete removes.
    public let files: [String]
    public let arch: String
    public let streamable: Bool
    public let complete: Bool
    public var id: String { path }

    public init(_ j: JSON) {
        name = j["name"].string(or: "")
        path = j["path"].string(or: "")
        dir = j["dir"].string(or: "")
        bytes = j["bytes"].int64(or: 0)
        shards = j["shards"].int(or: 1)
        files = j["files"].strings
        arch = j["arch"].string(or: "")
        streamable = j["streamable"].bool(or: false)
        complete = j["complete"].bool(or: true)
    }
}

/// One file of a catalog entry, with the URL it is fetched from. Sizes are exact.
public struct CatalogFile: Equatable, Sendable, Codable, Hashable {
    public let name: String
    public let bytes: Int64
    public let url: String

    public init(name: String, bytes: Int64, url: String) {
        self.name = name
        self.bytes = bytes
        self.url = url
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
    /// Added from a repository (`POST /api/hf/add`): it can be removed; its files stay.
    public let added: Bool

    public init?(_ j: JSON) {
        let files = (j["files"].array ?? []).map {
            CatalogFile(name: $0["name"].string(or: ""), bytes: $0["bytes"].int64(or: 0), url: $0["url"].string(or: ""))
        }
        guard let id = j["id"].string, !files.isEmpty else { return nil }
        self.id = id
        self.files = files
        title = j["title"].string(or: id)
        quant = j["quant"].string(or: "")
        blurb = j["blurb"].string(or: "")
        bytes = j["bytes"].int64 ?? files.reduce(0) { $0 + $1.bytes }
        serverStatus = j["status"].string(or: "available")
        added = j["added"].bool(or: false)
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

/// How the app writes a size: decimal GB, one decimal, the unit the model repositories quote.
/// Quoting a model as 17.0 GB and then refusing it for "15 GiB" reads as a bug though both are true.
public enum ByteText {
    public static func gb(_ bytes: Int64) -> String { String(format: "%.1f GB", Double(bytes) / 1e9) }
    public static func mib(_ mib: Double) -> String {
        mib >= 1024 ? String(format: "%.1f GiB", mib / 1024) : String(format: "%.0f MiB", mib)
    }
}
