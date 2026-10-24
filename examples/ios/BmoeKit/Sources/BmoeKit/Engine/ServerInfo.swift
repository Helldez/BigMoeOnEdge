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
    /// The session's context against what a client of the server sends; nil from a server that
    /// does not say.
    public let ctxAdvice: CtxAdvice?

    /// `ctx_advice`: what a coding agent's first request needs, whether the context is short of
    /// it, and the parameter that raises it. The app names neither the number nor the key.
    public struct CtxAdvice: Equatable, Sendable {
        public let recommended: Int
        public let short: Bool
        public let param: String
    }

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
        let a = j["ctx_advice"]
        if let rec = a["recommended"].int, let param = a["param"].string, !param.isEmpty {
            ctxAdvice = CtxAdvice(recommended: rec, short: a["short"].bool(or: false), param: param)
        } else {
            ctxAdvice = nil
        }
    }
}

/// What the screens say about the loaded session's context against what a client of the server
/// sends (docs/screens.md, "The status line"): the figures and the setting all come from the
/// server, the setting's name from the parameter table.
public struct ContextAdvice: Equatable, Sendable {
    /// The loaded session's context, in tokens.
    public let nCtx: Int
    /// What a coding agent's first request needs, with room for its answer.
    public let recommended: Int
    /// The context is below the recommendation: a client like that is refused.
    public let short: Bool
    /// The parameter that raises it, for a link to its row in Settings.
    public let key: String
    /// That parameter as the table labels it.
    public let label: String

    /// The advice for `model`, or nil without a model or without the server's advice.
    public init?(model: ModelInfo?, params: [ParamSpec]) {
        guard let model, let a = model.ctxAdvice else { return nil }
        nCtx = model.nCtx
        recommended = a.recommended
        short = a.short
        key = a.param
        label = params.first { $0.key == a.param }?.label ?? a.param
    }

    /// The warning beside the model's name.
    public var warning: String {
        "Context \(nCtx) tokens, under the \(recommended) a coding agent's first request needs; a larger context "
            + "takes memory from the expert cache."
    }

    /// What a chat says when the conversation filled the context.
    public var fullText: String {
        "This conversation fills the model's context of \(nCtx) tokens. Start a new chat, or raise \(label) and "
            + "reload the model"
            + (short ? " (\(recommended) tokens fits a coding agent's first request; it takes memory from the expert cache)." : ".")
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
