import Foundation

/// The Models screen's one list (docs/screens.md, "Models"): the files on the device and the
/// catalog's entries, merged by file, each row in one state with one primary action. What a row
/// says comes from `GET /api/models` and, since the app downloads for itself here, from the app's
/// own transfers; nothing names a model, a size or an architecture. The web UI's `libraryRows`.
public enum Library {
    public enum RowState: Equatable, Sendable {
        /// In the catalog, nothing of it here yet: Download.
        case available
        /// Its download runs: progress and Cancel.
        case downloading
        /// A stopped download left part of it: Resume, or Delete partial.
        case partial
        /// On the device, ready to load.
        case onDevice
        /// The model the engine has open: Reload, Unload, Go to Chat.
        case loaded
        /// A split set with shards missing: not loadable; its entry, where it has one, fetches the rest.
        case incomplete
    }

    public struct Row: Equatable, Sendable, Identifiable {
        /// Stable across refreshes: the local path, or `catalog:<id>` for an entry with nothing here.
        public let id: String
        public let title: String
        public let local: LocalModel?
        public let entry: CatalogEntry?
        public let state: RowState
        /// What a stopped download of the entry already has here; nil when nothing can be resumed.
        public let partialBytes: Int64?
        /// The catalog's suggestion, nothing of it here and nothing started: listed after the others.
        public let suggested: Bool
        /// The listing's own name for the model (`folder/name`), for a delete; nil when the server
        /// would not delete it (a scan directory's model, loaded in place, or nothing on the device).
        public let deleteName: String?
    }

    /// The listing's name for a model the server would delete, or nil for one outside its models directory.
    public static func deleteName(_ m: LocalModel, modelsDir: String) -> String? {
        m.dir == modelsDir ? (m.folder.isEmpty ? m.name : "\(m.folder)/\(m.name)") : nil
    }

    /// The local model an entry's download made: its first file, at the top of the models directory.
    static func matches(_ m: LocalModel, _ e: CatalogEntry, modelsDir: String) -> Bool {
        m.dir == modelsDir && m.folder.isEmpty && m.name == e.file
    }

    /// The merged list: one row per model on the device (the catalog entry that made it folded in),
    /// then the entries whose download has begun, then the suggestions. Companion files (a
    /// projector, a draft head) are not rows: pass `local` already split (`FileRole.split`).
    /// `downloading` names the entries whose transfers run; `partials` what stopped ones kept.
    public static func rows(_ list: ModelsList, local: [LocalModel], downloading: Set<String>,
                            partials: [String: Int64], loadedPath: String?) -> [Row]
    {
        var rows: [Row] = []
        var claimed: Set<String> = []
        for m in local {
            let entry = list.catalog.first { !claimed.contains($0.id) && matches(m, $0, modelsDir: list.modelsDir) }
            if let entry { claimed.insert(entry.id) }
            let part = entry.flatMap { partials[$0.id] }
            let state: RowState
            if m.path == loadedPath {
                state = .loaded
            } else if let entry, downloading.contains(entry.id) {
                state = .downloading
            } else if !m.complete {
                state = part != nil ? .partial : .incomplete
            } else {
                state = .onDevice
            }
            rows.append(Row(id: m.path, title: entry?.title ?? m.name, local: m, entry: entry, state: state,
                            partialBytes: part, suggested: false, deleteName: deleteName(m, modelsDir: list.modelsDir)))
        }
        var started: [Row] = []
        var suggested: [Row] = []
        for e in list.catalog where !claimed.contains(e.id) {
            let part = partials[e.id]
            let state: RowState
            switch list.status(of: e, downloading: downloading) {
            case .downloading: state = .downloading
            case .onDevice: state = .onDevice
            case .available: state = part != nil ? .partial : .available
            }
            let row = Row(id: "catalog:\(e.id)", title: e.title, local: nil, entry: e, state: state, partialBytes: part,
                          suggested: state == .available && !e.added, deleteName: nil)
            if row.suggested { suggested.append(row) } else { started.append(row) }
        }
        return rows + started + suggested
    }

    /// The running downloads at a glance, for the status line: how many, and the share done over
    /// every one whose size is known (nil when none is). Nil when none runs.
    public static func downloadSummary(_ downloads: [String: DownloadProgress]) -> (count: Int, fraction: Double?)? {
        var count = 0
        var received: Int64 = 0
        var total: Int64 = 0
        for d in downloads.values where d.state == .running {
            count += 1
            if d.total > 0 {
                received += min(d.received, d.total)
                total += d.total
            }
        }
        guard count > 0 else { return nil }
        return (count, total > 0 ? Double(received) / Double(total) : nil)
    }

    /// The status line's words for it: "Downloading 42%", "2 downloads 42%".
    public static func downloadText(count: Int, fraction: Double?) -> String {
        let head = count == 1 ? "Downloading" : "\(count) downloads"
        return fraction.map { head + String(format: " %.0f%%", $0 * 100) } ?? head
    }
}
