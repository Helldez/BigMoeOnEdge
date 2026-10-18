import Foundation

/// A model from a Hugging Face repository, as the server reports it (docs/server-api.md, "Models"):
/// what a repository holds, what one model is before a byte of it is downloaded, and the catalog
/// entry it became. The app shows these and downloads the entry's files itself, as it does every
/// catalog entry; nothing here reads a gguf or names an engine parameter.
public enum HF {
    /// The body of a search: the text as typed, a token where given.
    public static func searchBody(query: String, token: String) -> JSON {
        var o: [String: JSON] = ["query": .string(query.trimmingCharacters(in: .whitespacesAndNewlines))]
        let t = token.trimmingCharacters(in: .whitespacesAndNewlines)
        if !t.isEmpty { o["token"] = .string(t) }
        return .object(o)
    }

    /// The body of the three requests: the repository as typed, the model where one is meant, a
    /// token where given.
    public static func body(repo: String, name: String = "", token: String) -> JSON {
        var o: [String: JSON] = ["repo": .string(repo.trimmingCharacters(in: .whitespacesAndNewlines))]
        if !name.isEmpty { o["name"] = .string(name) }
        let t = token.trimmingCharacters(in: .whitespacesAndNewlines)
        if !t.isEmpty { o["token"] = .string(t) }
        return .object(o)
    }
}

/// One repository a search found (`POST /api/hf/search`).
public struct HFRepoHit: Equatable, Sendable, Identifiable {
    public let id: String
    public let downloads: Int64
    public let likes: Int64
    public let gated: Bool
}

public struct HFSearch: Equatable, Sendable {
    public let query: String
    public let repos: [HFRepoHit]

    public init(_ j: JSON) {
        query = j["query"].string(or: "")
        repos = (j["repos"].array ?? []).map {
            HFRepoHit(id: $0["id"].string(or: ""), downloads: $0["downloads"].int64(or: 0), likes: $0["likes"].int64(or: 0),
                      gated: $0["gated"].bool(or: false))
        }
    }
}

/// One model of a repository: a split set is one model named by its first shard.
public struct HFModel: Equatable, Sendable, Identifiable {
    public let name: String
    public let quant: String
    public let bytes: Int64
    public let count: Int
    public let complete: Bool
    public var id: String { name }
}

/// `POST /api/hf/models`.
public struct HFModels: Equatable, Sendable {
    public let repo: String
    public let revision: String
    /// The file the request's URL named, "" when it named none.
    public let file: String
    public let models: [HFModel]

    public init(_ j: JSON) {
        repo = j["repo"].string(or: "")
        revision = j["revision"].string(or: "main")
        file = j["file"].string(or: "")
        models = (j["models"].array ?? []).map {
            HFModel(name: $0["name"].string(or: ""), quant: $0["quant"].string(or: ""), bytes: $0["bytes"].int64(or: 0),
                    count: $0["count"].int(or: 1), complete: $0["complete"].bool(or: true))
        }
    }
}

/// `POST /api/hf/inspect`: the facts the server read off a model's header, and its verdict here.
public struct HFFacts: Equatable, Sendable {
    public let name: String
    public let quant: String
    public let bytes: Int64
    public let files: Int
    public let complete: Bool
    public let readable: Bool
    public let arch: String
    public let isMoE: Bool
    public let nExpert: Int
    public let nExpertUsed: Int
    public let nLayer: Int
    public let denseBytes: Int64
    public let expertBytes: Int64
    public let streamable: Bool
    public let refusal: String
    /// False only when the disk cannot take the download.
    public let downloadable: Bool
    /// `streams`, `plain mmap` or `not readable`.
    public let mode: String
    public let disk: String
    public let memory: String

    public init(_ j: JSON) {
        let m = j["model"], v = j["verdict"]
        name = j["name"].string(or: "")
        quant = j["quant"].string(or: "")
        bytes = j["bytes"].int64(or: 0)
        files = j["files"].int(or: 1)
        complete = j["complete"].bool(or: true)
        readable = m["readable"].bool(or: false)
        arch = m["arch"].string(or: "")
        isMoE = m["is_moe"].bool(or: false)
        nExpert = m["n_expert"].int(or: 0)
        nExpertUsed = m["n_expert_used"].int(or: 0)
        nLayer = m["n_layer"].int(or: 0)
        denseBytes = m["dense_bytes"].int64(or: 0)
        expertBytes = m["expert_bytes"].int64(or: 0)
        streamable = m["streamable"].bool(or: false)
        refusal = m["refusal"].string(or: "")
        downloadable = v["downloadable"].bool(or: false)
        mode = v["mode"].string(or: "")
        disk = v["disk"].string(or: "")
        memory = v["memory"].string(or: "")
    }
}
