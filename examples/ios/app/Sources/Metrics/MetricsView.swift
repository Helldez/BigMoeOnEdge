import BmoeKit
import Charts
import SwiftUI

/// Metrics (docs/screens.md): what the engine measured about its own work, live and recorded.
struct MetricsView: View {
    @EnvironmentObject private var engine: Engine
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
            if files.isEmpty {
                Text("A file per loaded model appears here once it has answered, with Metrics CSV on in Settings.")
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

/// The live card: the current or last generation's rate, each token's wall time split into the four
/// terms (compute, flash wait, cache management, unattributed), and the run's figures once it ends.
struct LiveCard: View {
    @EnvironmentObject private var engine: Engine

    var body: some View {
        let t = engine.live
        let b = t.breakdown
        VStack(alignment: .leading, spacing: Grid.m) {
            SectionTitle("Live", trailing: t.step == 0 ? nil : b.isAverage ? "run average" : "token \(t.step)")
            if t.step == 0 {
                Text("The next answer's figures appear here as it is generated.").font(.ui(15)).foregroundStyle(Palette.muted)
            } else {
                HStack(alignment: .firstTextBaseline) {
                    Text(String(format: "%.2f", t.rate)).font(.ui(40, bold: true)).foregroundStyle(Palette.accentInk)
                    Text("tok/s").font(.ui(16)).foregroundStyle(Palette.muted)
                }
                if engine.info?.model?.streaming == false {
                    Text("Streaming is off for this model: the engine runs it as plain mmap, so there is no flash term.")
                        .font(.ui(12)).foregroundStyle(Palette.muted)
                }
                term("compute", b.computeMs, Palette.accent)
                term("flash wait", b.flashWaitMs, Palette.warn)
                term("cache mgmt", b.mgmtMs, Palette.accentInk)
                term("unattributed", b.unattributedMs, Palette.muted)
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], alignment: .leading, spacing: Grid.m) {
                    Figure(value: String(format: "%.0f ms", b.wallMs), label: "per token")
                    Figure(value: b.cpuBusyPct >= 0 ? String(format: "%.0f%%", b.cpuBusyPct) : "–", label: "CPU busy")
                    Figure(value: b.faultsPerToken >= 0 ? String(format: "%.1f", b.faultsPerToken) : "–", label: "faults / token")
                    Figure(value: t.cacheHitPct >= 0 ? String(format: "%.0f%%", t.cacheHitPct) : "–", label: "cache hits")
                    if t.finished {
                        Figure(value: t.prefillTps > 0 ? String(format: "%.1f", t.prefillTps) : "–", label: "prefill tok/s")
                        Figure(value: t.ttftSeconds >= 0 ? String(format: "%.1f s", t.ttftSeconds) : "–", label: "first token")
                        Figure(value: t.readMiB >= 0 ? ByteText.mib(t.readMiB) : "–", label: "streamed")
                        Figure(value: t.cacheBudgetMiB > 0
                            ? "\(ByteText.mib(max(0, t.cacheResidentMiB))) / \(ByteText.mib(t.cacheBudgetMiB))" : "–",
                            label: "cache resident / budget")
                        if t.mtpDrafted > 0 {
                            Figure(value: String(format: "%.0f%%", t.mtpAcceptancePct), label: "guesses kept")
                            Figure(value: String(format: "%.1f", t.effectiveTokensPerSecond), label: "real tok/s")
                        }
                    }
                }
            }
        }
        .card()
    }

    private func term(_ name: String, _ ms: Double, _ color: Color) -> some View {
        let total = engine.live.breakdown.totalMs
        return VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(name).font(.ui(14)).foregroundStyle(Palette.text)
                Spacer()
                Text(String(format: "%.0f ms · %.0f%%", ms, total > 0 ? 100 * ms / total : 0))
                    .font(.ui(14)).foregroundStyle(Palette.muted).monospacedDigit()
            }
            Meter(fraction: total > 0 ? ms / total : 0, color: color, height: 10)
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
                ShareLink(item: url)
                Button(role: .destructive) {
                    try? FileManager.default.removeItem(at: url)
                    dismiss()
                } label: { Image(systemName: "trash") }
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
