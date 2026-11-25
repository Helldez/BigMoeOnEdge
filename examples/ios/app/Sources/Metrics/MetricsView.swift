import BmoeKit
import Charts
import SwiftUI

/// Metrics (docs/screens.md): what the engine measured about its own work, live and recorded.
struct MetricsView: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var preferences: Preferences
    @State private var files: [URL] = []

    var body: some View {
        Page {
            LiveCard()
            records
            if !engine.log.isEmpty { log }
        }
        .task { files = listFiles() }
        .refreshable { files = listFiles() }
        .onChange(of: engine.lastDone) { _ in files = listFiles() }
    }

    private var records: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            SectionTitle("Records", trailing: files.isEmpty ? nil : "\(files.count)")
            // The recording switch, where its files are: the same place on every front-end that records.
            Toggle(isOn: $preferences.metricsCSV) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Record").foregroundStyle(Palette.text)
                    Text("Write a per-token CSV for each model loaded from now on, to compare runs or attach to a report.")
                        .font(.ui(12)).foregroundStyle(Palette.muted)
                }
            }
            .tint(Palette.accent)
            if files.isEmpty {
                Text(preferences.metricsCSV
                     ? "A file per loaded model appears here once it has answered."
                     : "Turn on Record, then load a model and send a message.")
                    .font(.ui(15)).foregroundStyle(Palette.muted)
            }
            ForEach(files, id: \.self) { url in
                NavigationLink {
                    RecordView(url: url)
                } label: {
                    RecordRow(url: url)
                }
            }
        }
    }

    private var log: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            SectionTitle("Engine log")
            Text(engine.log.suffix(40).joined(separator: "\n"))
                .font(.code).foregroundStyle(Palette.muted).textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .card(padding: 12)
        }
    }

    private func listFiles() -> [URL] {
        let dir = engine.paths.metrics
        let urls = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
        return urls.filter { $0.pathExtension == "csv" }.sorted { $0.lastPathComponent > $1.lastPathComponent }
    }
}

/// The live card and the last answer's account, in the words the web UI's Metrics page and the
/// figures under an answer use: the rate now, waiting for the disk and what was found in memory
/// now; then where each token's time went in the last answer, as whole shares that add up to 100
/// (the same split as under the answer), and that answer's figures. No raw milliseconds per term:
/// the split says them.
struct LiveCard: View {
    @EnvironmentObject private var engine: Engine

    var body: some View {
        let t = engine.live
        let b = t.breakdown
        let done = engine.lastDone.map(TurnSummary.init)
        VStack(alignment: .leading, spacing: Grid.l) {
            VStack(alignment: .leading, spacing: Grid.m) {
                SectionTitle("Live", trailing: t.step == 0 ? nil : b.isAverage ? "run average" : "token \(t.step)")
                if t.step == 0 {
                    Text("The next answer's figures appear here as it is generated.").font(.ui(15)).foregroundStyle(Palette.muted)
                } else {
                    HStack(alignment: .firstTextBaseline) {
                        Text(String(format: "%.1f", t.rate)).font(.figure(40, bold: true)).foregroundStyle(Palette.accentInk)
                        Text("tok/s").font(.ui(16)).foregroundStyle(Palette.muted)
                    }
                    if let done { Text("Last answer: \(TurnFigures.fixed(done.tokS, 1)) tok/s on average").font(.ui(12)).foregroundStyle(Palette.muted) }
                    if engine.info?.model?.streaming == false {
                        Text("This model runs without streaming, so the disk and cache figures are not measured.")
                            .font(.ui(12)).foregroundStyle(Palette.muted)
                    }
                    if !t.finished, t.wallMs > 0 {
                        Text("Now: waiting for the disk \(Int((100 * t.flashWaitMs / t.wallMs).rounded()))% of each token's time")
                            .font(.ui(13)).foregroundStyle(Palette.muted)
                    }
                    LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], alignment: .leading, spacing: Grid.m) {
                        if !t.finished {
                            Figure(value: t.cacheHitPct < 0 ? "no cache" : "\(Int(t.cacheHitPct.rounded()))%",
                                   label: "Found in memory. Now, of the parts each token needs"
                                       + (t.cacheBudgetMiB > 0 ? " · cache \(ByteText.mib(t.cacheBudgetMiB))" : ""))
                        }
                        Figure(value: b.cpuBusyPct >= 0 ? String(format: "%.0f%%", b.cpuBusyPct) : "–", label: "CPU busy")
                        Figure(value: b.faultsPerToken >= 0 ? String(format: "%.1f", b.faultsPerToken) : "–", label: "faults / token")
                    }
                }
            }
            if let done {
                let split = TurnFigures.timeShares(done)
                if !split.isEmpty { TimeSplit(shares: split) }
                lastAnswer(done)
            }
        }
        .card()
    }

    /// The last answer's account, as the web UI's: its rows in the figures' words.
    private func lastAnswer(_ d: TurnSummary) -> some View {
        let t = engine.live
        let nCtx = engine.info?.model?.nCtx ?? 0
        var rows: [(String, String)] = [
            ("Tokens written", "\(t.step)" + (d.cancelled ? " (stopped)" : "")),
            ("Answer speed", "\(TurnFigures.fixed(d.tokS, 1)) tok/s"),
            ("Reading the message", "\(TurnFigures.fixed(d.promptTokS, 1)) tok/s, \(max(0, d.nPrompt)) tokens in \(TurnFigures.fixed(d.prefillS, 1)) s"),
            ("First word after", t.ttftSeconds >= 0 ? "\(TurnFigures.fixed(t.ttftSeconds, 2)) s" : "–"),
            ("Context used", nCtx > 0 ? "\(TurnFigures.grouped(max(0, d.nPast))) of \(TurnFigures.grouped(nCtx)) tokens"
                : TurnFigures.grouped(max(0, d.nPast))),
            ("Found in memory", d.cacheHitPct < 0 ? "no cache" : "\(Int(d.cacheHitPct.rounded())) of every 100 parts"),
            ("Read from disk", t.readMiB >= 0 ? ByteText.mib(t.readMiB) : "–"),
        ]
        // The cache's own account, which a field report could only read from the command line.
        if t.cacheBudgetMiB > 0 && t.cacheResidentMiB >= 0 {
            rows.append(("Cache in memory", "\(ByteText.mib(t.cacheResidentMiB)) of \(ByteText.mib(t.cacheBudgetMiB))"))
        }
        if t.cacheBudgetMiB > 0 && t.cacheRereadsPerToken >= 0 {
            rows.append(("Read again", "\(TurnFigures.fixed(t.cacheRereadsPerToken, 1)) parts per token"))
        }
        if t.readMiB > 0 && t.avgFlashWaitMs >= 0 {
            rows.append(("Waiting for disk", "\(TurnFigures.fixed(t.avgFlashWaitMs, 0)) ms per token"))
        }
        if d.mtpDrafted > 0 { rows.append(("Guesses kept", "\(d.mtpAccepted) of \(d.mtpDrafted) guesses")) }
        return VStack(alignment: .leading, spacing: Grid.s) {
            Text("Last answer").font(.ui(16, weight: .semibold)).foregroundStyle(Palette.text)
            ForEach(rows, id: \.0) { row in
                // The figures' rule: a long value under its label, left-aligned.
                Group {
                    if TurnFigure.valueOnOwnLine(row.1) {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(row.0).font(.ui(14)).foregroundStyle(Palette.muted)
                            Text(row.1).font(.figure(14)).foregroundStyle(Palette.text).fixedSize(horizontal: false, vertical: true)
                        }
                    } else {
                        HStack(alignment: .firstTextBaseline, spacing: Grid.m) {
                            Text(row.0).font(.ui(14)).foregroundStyle(Palette.muted)
                            Spacer(minLength: Grid.s)
                            Text(row.1).font(.figure(14)).foregroundStyle(Palette.text)
                        }
                    }
                }
                .accessibilityElement(children: .combine)
            }
        }
    }
}

/// Where each token's time went in the last answer: one bar of whole shares that add up to 100,
/// and its legend. The colours are the web's: computing in the accent, the disk in the warning's,
/// the cache in ok, other in the strong line.
struct TimeSplit: View {
    let shares: [TimeShare]

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Text("Last answer: where each token's time went").font(.ui(16, weight: .semibold)).foregroundStyle(Palette.text)
            GeometryReader { g in
                HStack(spacing: 2) {
                    ForEach(shares, id: \.key) { s in
                        Rectangle().fill(Self.colour(s.key))
                            .frame(width: max(0, (g.size.width - 2 * CGFloat(shares.count - 1)) * CGFloat(s.pct) / 100))
                    }
                }
            }
            .frame(height: 12)
            .background(Palette.surface2)
            .clipShape(Capsule())
            .accessibilityElement()
            .accessibilityLabel(shares.map { "\(Self.name($0.key)) \($0.pct)%" }.joined(separator: ", "))
            ForEach(shares, id: \.key) { s in
                HStack(spacing: Grid.s) {
                    RoundedRectangle(cornerRadius: 3).fill(Self.colour(s.key)).frame(width: 12, height: 12)
                    Text(Self.name(s.key)).font(.ui(14)).foregroundStyle(Palette.text)
                    Text("\(s.pct)%").font(.figure(14)).foregroundStyle(Palette.muted)
                }
                .accessibilityElement(children: .combine)
            }
        }
    }

    static func name(_ k: TimeShare.Key) -> String {
        switch k {
        case .compute: return "Computing"
        case .disk: return "Waiting for disk"
        case .mgmt: return "Managing the cache"
        case .other: return "Other"
        }
    }

    static func colour(_ k: TimeShare.Key) -> Color {
        switch k {
        case .compute: return Palette.accent
        case .disk: return Palette.warn
        case .mgmt: return Palette.ok
        case .other: return Palette.lineStrong
        }
    }
}

struct RecordRow: View {
    let url: URL

    var body: some View {
        let file = try? MetricsFile(contentsOf: url)
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(file?.title.isEmpty == false ? file!.title : url.lastPathComponent)
                    .foregroundStyle(Palette.text).lineLimit(1)
                Text(url.lastPathComponent).font(.ui(12)).foregroundStyle(Palette.muted)
            }
            Spacer()
            Image(systemName: "chevron.right").foregroundStyle(Palette.muted)
        }
        .padding(12)
        .box(lifted: false)
    }
}

/// One recorded run: what it was run with, what each turn did, and any column over its tokens.
struct RecordView: View {
    @EnvironmentObject private var engine: Engine
    let url: URL
    @State private var file: MetricsFile?
    @State private var column = "wall_ms"
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        Page {
            if let file {
                chart(file)
                turns(file)
                preamble(file)
            } else {
                Text("This file could not be read.").foregroundStyle(Palette.muted)
            }
        }
        .navigationTitle(url.lastPathComponent)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItemGroup(placement: .navigationBarTrailing) {
                ShareLink(item: url).tint(Palette.onShell)
                Button(role: .destructive) {
                    try? FileManager.default.removeItem(at: url)
                    dismiss()
                } label: { Image(systemName: "trash") }
                .tint(Palette.onShell)
            }
        }
        .task { file = try? MetricsFile(contentsOf: url) }
    }

    private func chart(_ f: MetricsFile) -> some View {
        let values = f.column(column) ?? []
        let points = values.enumerated().compactMap { i, v in v.map { (i + 1, $0) } }
        return VStack(alignment: .leading, spacing: Grid.s) {
            Picker("Column", selection: $column) {
                ForEach(f.plottable, id: \.self) { Text($0).tag($0) }
            }
            .pickerStyle(.menu)
            .tint(Palette.accent)
            if let field = MetricFields.of(column) {
                Text("\(field.short): \(field.measures)").font(.ui(12)).foregroundStyle(Palette.muted)
            }
            Chart(points, id: \.0) { p in
                LineMark(x: .value("token", p.0), y: .value(column, p.1))
                    .foregroundStyle(Palette.accentInk)
                    .interpolationMethod(.stepCenter)
            }
            .chartXAxisLabel("token")
            .frame(height: 200)
            if let s = ColumnStats(values) {
                HStack {
                    Figure(value: String(format: "%.2f", s.min), label: "min")
                    Figure(value: String(format: "%.2f", s.mean), label: "mean")
                    Figure(value: String(format: "%.2f", s.max), label: "max")
                }
            }
        }
        .card()
    }

    private func turns(_ f: MetricsFile) -> some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            SectionTitle("Turns", trailing: "\(f.summaries.count)")
            ForEach(Array(f.summaries.enumerated()), id: \.offset) { i, s in
                Text(["turn \(s["turn"] ?? "\(i + 1)")", s["tok/s"].map { "\($0) tok/s" }, s["tokens"].map { "\($0) tok" },
                      s["prefill_s"].map { "prefill \($0) s" }, s["cache_hit_pct"].map { "cache \($0)%" },
                      s["cancelled"] != nil ? "stopped" : nil].compactMap { $0 }.joined(separator: " · "))
                    .font(.ui(13)).foregroundStyle(Palette.text)
            }
        }
        .card()
    }

    /// What the run was opened with. A setting is explained by the parameter table, never a copy
    /// kept here; a key the table does not know is shown under its own name.
    private func preamble(_ f: MetricsFile) -> some View {
        let byKey = Dictionary(engine.params.map { (MetricsFile.preambleKey($0.key), $0) }, uniquingKeysWith: { a, _ in a })
        return VStack(alignment: .leading, spacing: Grid.s) {
            SectionTitle("Run with")
            ForEach(f.info.keys.sorted(), id: \.self) { k in
                HStack(alignment: .firstTextBaseline) {
                    Text(byKey[k]?.label ?? k).font(.ui(12)).foregroundStyle(byKey[k]?.lossy == true ? Palette.warn : Palette.muted)
                    Spacer()
                    Text(f.info[k] ?? "").font(.code).foregroundStyle(Palette.text)
                }
            }
        }
        .card()
    }
}
