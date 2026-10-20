import Foundation

/// The loaded session, as `GET /api/info` describes it.
public struct ModelInfo: Equatable, Sendable {
    public let name: String
    public let path: String
    public let arch: String
    public let nCtx: Int
    public let nExpertUsed: Int
    /// How thinking is switched: `template`, `prefill` or `none`.
    public let thinkControl: String
    public let loadSeconds: Double
    /// Whether experts stream from flash; off, the engine is plain llama.cpp on mmap.
    public let streaming: Bool
    /// What a CPU figure is read against: the session's compute threads, and every busy thread.
    public let computeThreads: Int
    public let busyThreads: Int

    public init?(_ j: JSON) {
        guard j.object != nil else { return nil }
        name = j["name"].string(or: "")
        path = j["path"].string(or: "")
        arch = j["arch"].string(or: "")
        nCtx = j["n_ctx"].int(or: 0)
        nExpertUsed = j["n_expert_used"].int(or: 0)
        thinkControl = j["think_ctl"].string(or: "template")
        loadSeconds = j["load_s"].double(or: 0)
        streaming = j["streaming"].bool(or: false)
        computeThreads = j["compute_threads"].int(or: 0)
        busyThreads = j["busy_threads"].int(or: 0)
    }
}

/// What the engine is doing (docs/screens.md, "The status line").
public enum EngineState: String, Sendable {
    case empty, planning, loading, ready, error
}

/// `GET /api/info`, and every `state` event.
public struct ServerInfo: Equatable, Sendable {
    public let version: String
    public let plannerAvailable: Bool
    public let state: EngineState
    public let error: String
    public let model: ModelInfo?
    public let generating: Bool
    public let ramTotalMiB: Int64
    public let ramAvailableMiB: Int64
    public let cpuThreads: Int

    public init(_ j: JSON) {
        version = j["version"].string(or: "")
        plannerAvailable = j["planner_available"].bool(or: false)
        state = EngineState(rawValue: j["state"].string(or: "empty")) ?? .empty
        error = j["error"].string(or: "")
        model = ModelInfo(j["model"])
        generating = j["generating"].bool(or: false)
        ramTotalMiB = j["host"]["ram_total_mib"].int64(or: 0)
        ramAvailableMiB = j["host"]["ram_available_mib"].int64(or: 0)
        cpuThreads = j["host"]["cpu_threads"].int(or: 0)
    }

    public var ready: Bool { state == .ready && model != nil }
    public var busyLoading: Bool { state == .loading || state == .planning }
}
