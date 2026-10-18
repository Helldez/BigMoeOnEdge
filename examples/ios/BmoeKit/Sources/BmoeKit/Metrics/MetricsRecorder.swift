import Foundation

/// Writes one per-token CSV per loaded session from the server's `token` and `done` events, in the
/// shape the engine's own `--csv` sink writes and the Android app writes: a `#` preamble with what
/// the session was opened with, one row per token, and a `# summary` line per turn. One file per
/// model loaded, which is the unit a comparison is made of.
public final class MetricsRecorder: @unchecked Sendable {
    /// The token event's columns, in the engine sink's order where the two share a name.
    public static let columns = [
        "step", "steps", "wall_ms", "io_ms", "compute_ms", "mgmt_ms", "stall_ms", "flash_wait_ms", "read_mib",
        "cache_hit_pct", "cache_budget_mib", "rss_mib", "mem_available_mib", "majflt", "cpu_ms",
        "dense_resident_frac", "mtp_batch",
    ]

    private let dir: URL
    private let lock = NSLock()
    private var handle: FileHandle?
    private var turn = 0
    private var headerWritten = false

    public init(directory: URL) {
        dir = directory
    }

    /// A session opened: a new file, its preamble written. The first line is what the server said
    /// about the session; the second every configuration value, under the parameter's key with `_`
    /// for `-`, so a file states what it was run with and the parameter table can explain it.
    public func sessionOpened(info: ServerInfo, config: ConfigState?, now: Date = Date()) {
        lock.lock()
        defer { lock.unlock() }
        closeLocked()
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyyMMdd-HHmmss"
        let url = dir.appendingPathComponent("bmoe-\(f.string(from: now)).csv")
        guard FileManager.default.createFile(atPath: url.path, contents: nil), let h = try? FileHandle(forWritingTo: url) else {
            return
        }
        handle = h
        turn = 0
        headerWritten = false
        var head = "# bmoe_metrics v=app engine=\(info.version)"
        if let m = info.model {
            head += " model=\(m.name.replacingOccurrences(of: " ", with: "_")) arch=\(m.arch) n_ctx=\(m.nCtx)"
            head += " n_expert_used=\(m.nExpertUsed) streaming=\(m.streaming ? 1 : 0)"
            head += " compute_threads=\(m.computeThreads) busy_threads=\(m.busyThreads)"
        }
        write(head + "\n")
        if let config {
            var line = "#"
            for (k, v) in config.values.sorted(by: { $0.key < $1.key }) where k != "model" && !v.isNull {
                let value: String
                if let b = v.bool { value = b ? "1" : "0" } else { value = v.display.replacingOccurrences(of: " ", with: "_") }
                line += " \(MetricsFile.preambleKey(k))=\(value)"
            }
            write(line + "\n")
        }
    }

    public func token(_ o: JSON) {
        lock.lock()
        defer { lock.unlock() }
        guard handle != nil else { return }
        if !headerWritten {
            write(Self.columns.joined(separator: ",") + ",turn\n")
            headerWritten = true
            turn += 1
        } else if o["step"].int == 1 {
            turn += 1
        }
        write(Self.columns.map { cell(o[$0]) }.joined(separator: ",") + ",\(turn)\n")
    }

    public func done(_ o: JSON) {
        lock.lock()
        defer { lock.unlock() }
        guard handle != nil else { return }
        var line = "# summary turn=\(turn) tokens=\(o["tokens"].int(or: 0))"
        line += String(format: " tok/s=%.3f prefill_s=%.3f", o["tok_s"].double(or: 0), o["prefill_s"].double(or: 0))
        line += " n_prompt=\(o["n_prompt"].int(or: 0)) n_past=\(o["n_past"].int(or: 0))"
        line += String(format: " cache_hit_pct=%.1f read_mib=%.1f", o["cache_hit_pct"].double(or: -1), o["read_mib"].double(or: 0))
        line += String(format: " majflt_tok=%.2f cpu_s_tok=%.4f", o["majflt_tok"].double(or: 0), o["cpu_s_tok"].double(or: 0))
        if o["cancelled"].bool(or: false) { line += " cancelled=1" }
        write(line + "\n")
        try? handle?.synchronize()
    }

    public func close() {
        lock.lock()
        defer { lock.unlock() }
        closeLocked()
    }

    private func closeLocked() {
        try? handle?.close()
        handle = nil
    }

    private func write(_ s: String) { try? handle?.write(contentsOf: Data(s.utf8)) }

    private func cell(_ v: JSON) -> String {
        switch v {
        case .null: return ""
        case let .double(d) where d.isFinite && d != d.rounded(): return String(format: "%.4f", d)
        default: return v.display
        }
    }
}
