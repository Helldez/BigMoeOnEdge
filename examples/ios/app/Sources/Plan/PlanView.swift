import BmoeKit
import SwiftUI

/// Plan (docs/screens.md): what the hardware planner measured on this device and what it decided
/// for the chosen model, read top down as the web UI and the Android app read it: the verdict and
/// its three figures, Apply and Measure again, where the memory goes, what each device does, the
/// decisions grouped, the command. One layout for every machine and the same one on every
/// front-end: each block is a list of whatever the plan holds, and every word in it is the
/// server's; a code beside a word only picks a colour. The plan itself lives in the engine, which
/// reads it again when a session opens and when a turn ends, so this screen only draws it.
struct PlanView: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var nav: Navigator
    @State private var applying = false
    /// What the last Apply changed, row by row; nil before one.
    @State private var applied: [AppliedChange]?
    /// Why Apply failed; a failed read or measurement is the engine's `planError`.
    @State private var failure: String?
    @State private var copied = false

    private var plan: PlanResult? { engine.plan }
    private var measuring: Bool { engine.measuring }
    /// One banner for whatever went wrong: the request, Apply, or a plan the server could not make.
    private var shownError: String? {
        if let failure { return failure }
        if let e = engine.planError { return e }
        if let p = plan, p.available, !p.error.isEmpty { return p.error }
        return nil
    }

    static let planningDoc = URL(string: "https://github.com/Helldez/BigMoeOnEdge/blob/main/docs/hardware-planning.md")!

    private var planner: Bool { engine.info?.plannerAvailable ?? true }
    /// A model chosen or loaded: there is something to plan for.
    private var chosen: Bool { SettingsForm.loadTargetChosen(engine.params, config: engine.config, info: engine.info) }
    private var canMeasure: Bool {
        planner && chosen && !measuring && engine.serverUp && !engine.generating && !engine.loading
    }

    var body: some View {
        Page {
            if !planner { Banner(text: "This build does not include the hardware planner.", color: Palette.err) }
            if !chosen, !engine.params.isEmpty {
                Text("Choose a model from Models: a plan is for one model on this device.")
                    .font(.subheadline).foregroundStyle(Palette.muted)
            }
            if let shownError {
                // A failed plan can be measured again from where it is said.
                Banner(text: shownError, color: Palette.err) {
                    if planner { measureButton(primary: false) }
                }
            }
            if measuring || engine.planProgress != nil { PlanProgressCard(progress: engine.planProgress) }
            if let p = plan, p.available, engine.config != nil { autoPlan }
            content
        }
        // The engine reads the plan when it reaches the server, when a session opens and when a turn
        // ends: never a measurement, which on a phone takes a while and must not start on its own.
        // A screen opened before the engine got there asks once.
        .task(id: engine.serverUp) {
            if engine.serverUp, engine.plan == nil { await engine.refreshPlan() }
        }
    }

    @ViewBuilder
    private var content: some View {
        if let p = plan {
            if !p.available {
                Banner(text: p.error.isEmpty ? "No plan." : p.error, color: Palette.err)
                lastApplied
            } else if !p.error.isEmpty {
                lastApplied // the error is in the banner above
            } else if p.none {
                VStack(alignment: .leading, spacing: Grid.s) {
                    Text("Not measured yet.").font(.pixel(22, bold: true)).foregroundStyle(Palette.text)
                    Text("Measure this device to see what the plan does with the chosen model. The probes read "
                        + "storage and run a matmul on every device; on a phone that takes a while.")
                        .font(.subheadline).foregroundStyle(Palette.muted)
                    Button(measuring ? "Measuring…" : "Measure") { Task { await measure() } }
                        .buttonStyle(.pixelPrimary).disabled(!canMeasure)
                }
                .card()
                lastApplied
            } else {
                planBody(p)
            }
        } else {
            lastApplied
        }
    }

    /// With nothing measured to show, what the configuration carries from the last applied plan, so
    /// the screen is never empty after a plan has been made.
    @ViewBuilder
    private var lastApplied: some View {
        if let decisions = engine.config?.plan, !decisions.isEmpty {
            SectionTitle("Last applied plan")
            PlanDecisions(decisions: decisions, openSetting: nav.openSetting)
        }
    }

    private var autoPlan: some View {
        Toggle(isOn: Binding(get: { engine.config?.autoPlan ?? false },
                             set: { on in Task { try? await engine.setAutoPlan(on) } }))
        {
            VStack(alignment: .leading, spacing: 2) {
                Text("Plan before every load").font(.subheadline).foregroundStyle(Palette.text)
                Text("The previous model is unloaded first. Values you set yourself are kept.")
                    .font(.caption).foregroundStyle(Palette.muted)
            }
        }
        .tint(Palette.accent)
        .disabled(!planner || engine.config == nil)
    }

    private func measureButton(primary: Bool) -> some View {
        Button(measuring ? "Measuring…" : "Measure again") { Task { await measure() } }
            .buttonStyle(primary ? .pixelPrimary : .pixel).disabled(!canMeasure)
    }

    @ViewBuilder
    private func planBody(_ p: PlanResult) -> some View {
        if !p.warning.isEmpty { Banner(text: p.warning) }
        summary(p)
        if !p.pools.isEmpty {
            SectionTitle("Memory")
            VStack(alignment: .leading, spacing: Grid.l) { ForEach(p.pools.indices, id: \.self) { PoolBar(pool: p.pools[$0]) } }
                .card()
        }
        if !p.devices.isEmpty {
            SectionTitle("Devices")
            VStack(alignment: .leading, spacing: Grid.m) {
                let scale = max(1, p.devices.map(\.promptRatio).max() ?? 1)
                ForEach(p.devices.indices, id: \.self) { DeviceRow(device: p.devices[$0], scale: scale) }
                if !p.devicesNoneText.isEmpty {
                    Text(p.devicesNoneText).font(.caption).foregroundStyle(Palette.muted)
                }
            }
            .card()
        }
        SectionTitle("Decisions")
        PlanDecisions(decisions: p.decisions, openSetting: nav.openSetting)
        if !p.args.isEmpty { Fold(title: "Command") { CommandText(text: p.reproduceLine) } }
        // The planner's whole account is copied for a bug report, not shown: every line of it has
        // a worded twin on this screen.
        HStack(spacing: Grid.s) {
            Button(copied ? "Copied" : "Copy diagnostic") {
                UIPasteboard.general.string = p.diagnostic
                copied = true
            }
            .buttonStyle(.pixel)
            Text("The command and the planner's whole account, for a bug report.").font(.caption).foregroundStyle(Palette.muted)
        }
        Link("How the planner decides", destination: Self.planningDoc).font(.caption).foregroundStyle(Palette.accent)
    }

    private func summary(_ p: PlanResult) -> some View {
        let f = p.figures
        return VStack(alignment: .leading, spacing: 12) {
            if p.fromLastLoad {
                Badge(text: "The loaded model runs this plan", color: Palette.ok)
            } else if p.cached {
                Badge(text: "The last plan measured")
            }
            Text(p.sentence.isEmpty ? "Regime: \(p.regime)" : p.sentence)
                .font(.pixel(22, bold: true)).foregroundStyle(Palette.text).fixedSize(horizontal: false, vertical: true)
            if !p.machine.isEmpty { Text(p.machine).font(.caption).foregroundStyle(Palette.muted) }
            if p.streamingDeclined, !p.declineReason.isEmpty {
                Text(p.declineReason).font(.caption).foregroundStyle(Palette.warn)
            }
            if !p.lanes.isEmpty {
                // Which device computes the prompt and which the tokens, in one line.
                lanesText(p.lanes).font(.pixel(15)).fixedSize(horizontal: false, vertical: true)
            }
            if !p.notApplicable.isEmpty {
                Text("Decided, but not a setting this app can hold: " + p.notApplicable.joined(separator: "; "))
                    .font(.caption).foregroundStyle(Palette.warn)
            }
            LazyVGrid(columns: [GridItem(.flexible(), spacing: Grid.s, alignment: .top), GridItem(.flexible(), alignment: .top)],
                      alignment: .leading, spacing: Grid.s)
            {
                speedTile(p, f)
                if let shown = f.measuredHitPct ?? f.creditedHitPct { hitsTile(f, shown) }
                if let held = f.heldBytes, let cap = f.capBytes { memoryTile(held, cap) }
            }
            HStack(spacing: Grid.s) {
                Button(applying ? "Applying…" : "Apply plan") { Task { await apply() } }
                    .buttonStyle(.pixelPrimary).disabled(applying || measuring)
                measureButton(primary: false)
            }
            Text("Values you set yourself are never changed.").font(.caption).foregroundStyle(Palette.muted)
            if let applied { appliedList(applied) }
            if engine.config?.reloadRequired == true, engine.ready {
                HStack(spacing: Grid.s) {
                    Text("Some applied values need a model reload.").font(.caption).foregroundStyle(Palette.muted)
                    Button("Settings") { nav.open(.settings) }.font(.pixel(13)).foregroundStyle(Palette.accent).buttonStyle(.borderless)
                }
            }
        }
        .card()
    }

    private func lanesText(_ lanes: [PlanResult.Lane]) -> Text {
        var t = Text("")
        for (i, l) in lanes.enumerated() {
            if i > 0 { t = t + Text("   ") }
            t = t + Text(l.label + " ").foregroundColor(Palette.muted) + Text(l.on.joined(separator: " + ")).foregroundColor(Palette.accent)
        }
        return t
    }

    /// What Apply changed, each row a way to its setting.
    @ViewBuilder
    private func appliedList(_ changes: [AppliedChange]) -> some View {
        if changes.isEmpty {
            Text("Plan applied: every setting already had the plan's value.").font(.caption).foregroundStyle(Palette.ok)
        } else {
            VStack(alignment: .leading, spacing: 6) {
                Text("Plan applied. \(changes.count == 1 ? "One setting changed" : "\(changes.count) settings changed"):")
                    .font(.caption).foregroundStyle(Palette.ok)
                ForEach(changes) { c in
                    Button { nav.openSetting(c.key) } label: {
                        HStack(alignment: .firstTextBaseline, spacing: 6) {
                            Text(c.label).font(.subheadline).foregroundStyle(Palette.accent).underline()
                            Text(c.from.isEmpty ? "" : c.from).font(.pixel(13)).foregroundStyle(Palette.muted)
                            Text("→ \(c.to)").font(.pixel(13)).foregroundStyle(Palette.text)
                            Spacer(minLength: 0)
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }

    private func speedTile(_ p: PlanResult, _ f: PlanResult.Figures) -> some View {
        Tile {
            Text("Tokens").font(.caption).foregroundStyle(Palette.muted)
            if let m = f.measuredTokS {
                Figure(value: Self.number(m), label: "tok/s measured")
                if let d = f.deltaPct, let e = f.expectedTokS {
                    if p.fromLastLoad {
                        HStack(spacing: 4) {
                            Badge(text: String(format: "%+.0f%%", d), color: d >= 0 ? Palette.ok : Palette.warn)
                            Text("against the \(Self.number(e)) expected").font(.caption).foregroundStyle(Palette.muted)
                        }
                    } else {
                        Text("\(Self.number(e)) tok/s expected under this plan").font(.caption).foregroundStyle(Palette.muted)
                    }
                }
            } else if let e = f.expectedTokS {
                Figure(value: Self.number(e), label: "tok/s expected")
                Text(p.fromLastLoad ? "Measured after the first answer" : "Measured after a load")
                    .font(.caption).foregroundStyle(Palette.muted)
            } else {
                Text("No prediction").font(.caption).foregroundStyle(Palette.muted)
            }
        }
    }

    private func hitsTile(_ f: PlanResult.Figures, _ shown: Double) -> some View {
        Tile {
            Text("Cache hits").font(.caption).foregroundStyle(Palette.muted)
            Figure(value: String(format: "%.0f%%", shown), label: f.measuredHitPct != nil ? "measured" : "credited")
            Meter(fraction: shown / 100, height: 10, mark: f.measuredHitPct != nil ? f.creditedHitPct.map { $0 / 100 } : nil)
            if f.measuredHitPct != nil, let credited = f.creditedHitPct {
                Text(String(format: "The mark is the %.0f%% the plan credited", credited))
                    .font(.caption).foregroundStyle(Palette.muted)
            }
        }
    }

    private func memoryTile(_ held: Int64, _ cap: Int64) -> some View {
        Tile {
            Text("Memory").font(.caption).foregroundStyle(Palette.muted)
            Figure(value: Bytes.label(held), label: "held")
            Meter(fraction: Double(held) / Double(cap), height: 10)
            Text("Of the \(Bytes.label(cap)) it may hold").font(.caption).foregroundStyle(Palette.muted)
        }
    }

    private func measure() async {
        failure = nil
        applied = nil
        await engine.measurePlan()
    }

    private func apply() async {
        applying = true
        applied = nil
        let before = engine.config?.values ?? [:]
        do {
            try await engine.applyPlan()
            applied = AppliedChange.between(before, engine.config?.values ?? [:], params: engine.params)
        } catch {
            failure = error.localizedDescription
        }
        applying = false
    }

    static func number(_ v: Double) -> String { String(format: "%.1f", v) }
}

/// A tile inside a card: the page colour, the edge, a figure and its words.
private struct Tile<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 4) { content }
            .padding(10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Palette.bg)
            .overlay(Rectangle().strokeBorder(Palette.border, lineWidth: Grid.edge))
    }
}

/// The command line that reproduces the plan, copyable, for a bug report.
private struct CommandText: View {
    let text: String
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Text(text).font(.code).foregroundStyle(Palette.text).textSelection(.enabled)
            Button(copied ? "Copied" : "Copy") {
                UIPasteboard.general.string = text
                copied = true
            }
            .buttonStyle(.pixel)
        }
    }
}

/// One memory pool as one bar: every row held for the run, then what is free, against the larger of
/// what the process may hold and the sum. A row held in turn with the cache is a sentence, not a
/// segment; a segment's colour is its position.
private struct PoolBar: View {
    let pool: PlanResult.Pool
    static let colours = [Palette.accent2, Palette.accent, Palette.muted, Palette.ok, Palette.warn]

    var body: some View {
        let held = pool.rows.filter { !$0.inTurn }
        let sum = held.reduce(Int64(0)) { $0 + $1.bytes }
        let whole = max(1, max(pool.capBytes, sum))
        let free = whole - sum
        VStack(alignment: .leading, spacing: Grid.s) {
            HStack(alignment: .firstTextBaseline) {
                Text(pool.label + (pool.estimated ? " (an even share of the devices' sum)" : ""))
                    .font(.subheadline).foregroundStyle(Palette.text)
                Spacer()
                if pool.capBytes > 0 {
                    Text("of " + Bytes.label(pool.capBytes)).font(.pixel(13)).foregroundStyle(Palette.muted)
                }
            }
            GeometryReader { g in
                HStack(spacing: 0) {
                    ForEach(held.indices, id: \.self) { i in
                        Rectangle().fill(Self.colours[i % Self.colours.count])
                            .frame(width: g.size.width * Double(held[i].bytes) / Double(whole))
                    }
                    Spacer(minLength: 0)
                }
            }
            .frame(height: 24)
            .background(Palette.surface2)
            .overlay(Rectangle().strokeBorder(Palette.border, lineWidth: Grid.edge))
            ForEach(held.indices, id: \.self) { i in
                HStack(spacing: 10) {
                    Rectangle().fill(Self.colours[i % Self.colours.count]).frame(width: 12, height: 12)
                    Text(held[i].label).font(.subheadline).foregroundStyle(Palette.text)
                    Spacer()
                    if held[i].locked { Badge(text: "locked") }
                    Text(Bytes.label(held[i].bytes)).font(.pixel(14)).foregroundStyle(Palette.text)
                }
            }
            if free > 0 {
                HStack(spacing: 10) {
                    Rectangle().strokeBorder(Palette.border, lineWidth: Grid.edge).frame(width: 12, height: 12)
                    Text("Free").font(.subheadline).foregroundStyle(Palette.text)
                    Spacer()
                    Text(Bytes.label(free)).font(.pixel(14)).foregroundStyle(Palette.text)
                }
            }
            ForEach(pool.rows.filter(\.inTurn).indices, id: \.self) { i in
                let r = pool.rows.filter(\.inTurn)[i]
                Text("\(r.label): \(Bytes.label(r.bytes)) borrowed from the expert cache while a prompt runs, given back after.")
                    .font(.caption).foregroundStyle(Palette.muted)
            }
        }
    }
}

/// A device the plan saw: its prompt rate as a bar scaled to the fastest, its badge in the server's
/// words coloured by its role alone, and why.
private struct DeviceRow: View {
    let device: PlanResult.Device
    let scale: Double

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .top, spacing: Grid.s) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(device.name).font(.pixel(16, bold: true)).foregroundStyle(Palette.text)
                    if !device.description.isEmpty, device.description != device.name {
                        Text(device.description).font(.caption).foregroundStyle(Palette.muted)
                    }
                }
                Spacer()
                Badge(text: device.badge, color: colour)
            }
            Meter(fraction: device.promptRatio > 0 ? max(0.02, device.promptRatio / scale) : 0,
                  color: device.status == "used" ? Palette.accent : Palette.muted, height: 10)
            if !device.rateText.isEmpty { Text(device.rateText).font(.pixel(13)).foregroundStyle(Palette.text) }
            if !device.why.isEmpty { Text(device.why).font(.caption).foregroundStyle(Palette.muted) }
        }
    }

    private var colour: Color {
        switch device.status {
        case "used": return Palette.ok
        case "refused", "unusable": return Palette.err
        default: return Palette.muted
        }
    }
}

/// The three groups: the settings the plan decided, open; the ones it left and the notes, folded,
/// each with a count and a line on what it holds.
struct PlanDecisions: View {
    let decisions: [PlanDecision]
    /// Settings, landed on the row of a knob.
    var openSetting: (String) -> Void

    var body: some View {
        let g = PlanDecision.grouped(decisions)
        if !g.set.isEmpty {
            Fold(title: "Set by the plan · \(g.set.count)", open: true) { rows(g.set) }
        }
        if !g.kept.isEmpty {
            Fold(title: "Left as is · \(g.kept.count)", hint: "Settings the plan looked at and left as they were.") {
                rows(g.kept)
            }
        }
        if !g.notes.isEmpty {
            Fold(title: "Notes · \(g.notes.count)", hint: "What the plan measured and weighed. None of these is a setting.") {
                rows(g.notes)
            }
        }
    }

    // Keyed by position: a knob can carry several decisions (a note topic can recur).
    private func rows(_ list: [PlanDecision]) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(list.indices, id: \.self) { DecisionRow(decision: list[$0], openSetting: openSetting) }
        }
    }
}

/// One decision: its label, value and source; on a tap the one-line brief, the full reason behind
/// Why, and the way to the setting.
private struct DecisionRow: View {
    let decision: PlanDecision
    var openSetting: (String) -> Void
    @State private var open = false
    @State private var why = false

    var body: some View {
        let d = decision
        VStack(alignment: .leading, spacing: 6) {
            Button { open.toggle() } label: {
                HStack(alignment: .firstTextBaseline, spacing: Grid.s) {
                    Image(systemName: open ? "chevron.down" : "chevron.right").font(.caption2.bold())
                        .foregroundStyle(Palette.muted).frame(width: 12)
                    Text(d.title).font(.subheadline).foregroundStyle(Palette.text).multilineTextAlignment(.leading)
                    Spacer(minLength: Grid.s)
                    if !d.shownValue.isEmpty {
                        Text(d.shownValue).font(.pixel(15, bold: true)).foregroundStyle(Palette.accent)
                            .multilineTextAlignment(.trailing)
                    }
                    Badge(text: d.source, color: Self.sourceColor(d.source))
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if open {
                VStack(alignment: .leading, spacing: 6) {
                    Text(d.brief.isEmpty ? d.reason : d.brief).font(.subheadline).foregroundStyle(Palette.text)
                        .fixedSize(horizontal: false, vertical: true)
                    if !d.brief.isEmpty, d.brief != d.reason, !d.reason.isEmpty {
                        Button { why.toggle() } label: {
                            Text(why ? "Why  ▾" : "Why  ▸").font(.pixel(13, bold: true)).foregroundStyle(Palette.accent)
                        }
                        .buttonStyle(.plain)
                        if why {
                            Text(d.reason).font(.caption).foregroundStyle(Palette.muted)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    // The setting itself: its card and fold opened, the field focused.
                    if !d.label.isEmpty, d.group != "notes" {
                        Button("Open \(d.title) in Settings") { openSetting(d.knob) }.buttonStyle(.pixel)
                    }
                }
                .padding(.leading, 20)
            }
        }
        .padding(.vertical, Grid.s)
    }

    static func sourceColor(_ s: String) -> Color {
        switch s {
        case "measured": return Palette.ok
        case "operator": return Palette.warn
        case "unprobed": return Palette.err
        default: return Palette.muted
        }
    }
}

/// A measurement in progress, in the server's words: the stages done, the one running, how many to
/// go. Before the first event it says only that it is measuring. Models shows it during a load too.
struct PlanProgressCard: View {
    let progress: PlanProgress?
    @State private var done: [String] = []
    @State private var last = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Measuring this device").font(.pixel(18, bold: true)).foregroundStyle(Palette.text)
            Meter(fraction: progress?.fraction ?? 0, height: 10)
            ForEach(done, id: \.self) { step(state: "done", colour: Palette.ok, label: $0) }
            if let p = progress {
                step(state: "running", colour: Palette.accent, label: p.label)
                if p.left > 0 { step(state: "next", colour: Palette.muted, label: "\(p.left) more") }
            }
        }
        .card()
        .onAppear { follow(progress) }
        .onChange(of: progress) { follow($0) }
    }

    private func follow(_ p: PlanProgress?) {
        guard let p else { return }
        if p.index == 0 { done = [] }
        if !last.isEmpty, last != p.label, !done.contains(last) { done.append(last) }
        last = p.label
    }

    private func step(state: String, colour: Color, label: String) -> some View {
        HStack(spacing: 10) {
            Badge(text: state, color: colour).frame(minWidth: 80, alignment: .leading)
            Text(label).font(.subheadline).foregroundStyle(Palette.text)
        }
    }
}

/// Binary units, as the web UI and the Android app show them, so a figure reads the same on all three.
enum Bytes {
    static func label(_ bytes: Int64) -> String {
        let units = ["B", "KiB", "MiB", "GiB", "TiB"]
        var v = Double(bytes)
        var u = 0
        while v >= 1024, u < units.count - 1 {
            v /= 1024
            u += 1
        }
        return String(format: u == 0 || v >= 100 ? "%.0f %@" : "%.1f %@", v, units[u])
    }
}
