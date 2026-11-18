import BmoeKit
import SwiftUI

/// The first run (docs/screens.md, "First run"): one path to the first private answer, in three real
/// steps. 1 Pick a model (one that fits in memory beside one larger than it); 2 its download, with
/// the time left; 3 the model loads by itself when the download ends, and the conversation opens
/// once the engine says it is ready. Shown on Models while the phone has no model and none is
/// coming; Skip, More models and Import a file stay one tap away. The app's own download, load and
/// catalog do the work; nothing here decides anything about the engine.
struct SetupView: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var nav: Navigator
    @EnvironmentObject private var chat: ChatModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    /// The rows Models built, so the entry's state is read the one way the list reads it.
    let rows: [Library.Row]
    var importFile: () -> Void

    @State private var pick: String?
    @State private var samples: [ETA.Sample] = []
    @State private var started = false
    @State private var actionError: String?
    /// The person asked to pick another model over a paused download, for this visit.
    @State private var repick = false
    /// What each picked model is on this device, by the server's check of its header: asked when a
    /// model becomes the pick, the suggested one included, and kept for the visit.
    @State private var checks: [String: CheckState] = [:]

    private var ramBytes: Int64 { (engine.info?.ramTotalMiB ?? 0) * 1024 * 1024 }
    private var catalog: [CatalogEntry] { engine.models.catalog }
    private var chosen: CatalogEntry? {
        let id = pick ?? nav.setupEntry
        return catalog.first { $0.id == id } ?? Setup.recommended(catalog, ramBytes: ramBytes)
    }
    private var row: Library.Row? { chosen.flatMap { e in rows.first { $0.entry?.id == e.id } } }
    private var step: Int { Setup.step(row?.state, loading: engine.loading, repick: repick) }
    /// The chosen model has part of it here and is not downloading: step 2 with Resume.
    private var paused: Bool { row?.state == .partial }

    var body: some View {
        VStack(spacing: Grid.l) {
            head
            StepsIndicator(current: step)
            sheet
            links
        }
        .frame(maxWidth: .infinity)
        .onChange(of: engine.downloads[chosen?.id ?? ""]?.received) { got in sample(got) }
        .onChange(of: step) { _ in startIfReady() }
        .onChange(of: row?.state) { _ in startIfReady() }
        .onAppear { startIfReady() }
        // The conversation opens when the model can answer, not when the load request returns.
        .onChange(of: engine.ready) { ready in
            guard ready, started else { return }
            nav.setupEntry = nil
            chat.newChat()
            nav.open(.chat)
        }
    }

    // MARK: the head

    private var head: some View {
        VStack(spacing: Grid.s) {
            SealedLogo(size: 64).padding(.bottom, Grid.s)
            Text("A private AI on this phone").font(.ui(24, bold: true)).foregroundStyle(Palette.text)
                .accessibilityAddTraits(.isHeader)
            Text("Big models run here, and nothing you write leaves this phone. Setting up is one download.")
                .font(.ui(16)).foregroundStyle(Palette.muted)
            let facts = Setup.facts(ramMiB: engine.info?.ramTotalMiB ?? 0, threads: engine.info?.cpuThreads ?? 0, gpus: gpus)
            if !facts.isEmpty { Text(facts).font(.ui(13)).foregroundStyle(Palette.muted) }
        }
        .multilineTextAlignment(.center)
        .padding(.top, Grid.m)
    }

    /// The graphics devices the plan names, where it has measured: what the app already knows.
    private var gpus: [String] {
        guard let p = engine.plan, p.available else { return [] }
        return p.devices.filter { !$0.cpu }.map { $0.description.isEmpty ? $0.name : $0.description }
    }

    // MARK: the sheet

    private var sheet: some View {
        VStack(alignment: .leading, spacing: Grid.m) {
            if !engine.modelsListed {
                Text(engine.serverUp ? "Reading the models…" : "The engine is not running.").font(.ui(15)).foregroundStyle(Palette.muted)
            } else if let e = chosen {
                switch step {
                case 1: pickStep(e)
                case 2: if paused { pausedStep(e) } else { downloadStep(e) }
                default: startStep(e)
                }
                if let actionError, step != 3 { Banner(text: actionError, color: Palette.err) }
            } else {
                Text("No model is suggested here. Find one in Models.").font(.ui(15)).foregroundStyle(Palette.text)
                Button("Models") { nav.setupDismissed = true }.buttonStyle(.appSecondary)
            }
        }
        .card()
        .accessibilityElement(children: .contain)
    }

    @ViewBuilder
    private func pickStep(_ chosen: CatalogEntry) -> some View {
        Text("Pick a model").font(.ui(18, bold: true)).foregroundStyle(Palette.text).accessibilityAddTraits(.isHeader)
        // Said before the list, with the two ways around it as buttons: the list is a suggestion,
        // and a gguf the person already has must not hide under the sheet.
        Text("Any gguf model runs here. Use one you already have, find another, or start from a suggestion.")
            .font(.ui(14)).foregroundStyle(Palette.text).fixedSize(horizontal: false, vertical: true)
        VStack(alignment: .leading, spacing: Grid.s) {
            Button("Use a .gguf file I already have", action: importFile)
            Button("Find a model on Hugging Face") { nav.setupDismissed = true }
        }
        .buttonStyle(.appSecondary)
        Text("Suggested for a first run").font(.ui(15, bold: true)).foregroundStyle(Palette.text).accessibilityAddTraits(.isHeader)
        let choices = Setup.choices(catalog)
        if !choices.isEmpty {
            // One under the other: a catalog's worth of them does not fit side by side.
            VStack(spacing: Grid.s) { ForEach(choices) { choice($0, selected: $0.id == chosen.id) } }
        } else {
            VStack(alignment: .leading, spacing: 4) {
                Text(chosen.title).font(.ui(16, bold: true)).foregroundStyle(Palette.text)
                Text(ByteText.gb(chosen.bytes)).font(.ui(13)).foregroundStyle(Palette.muted)
            }
        }
        checkPanel(chosen)
            .task(id: "\(chosen.id) \(engine.serverUp)") { await runCheck(chosen.id) }
        VStack(alignment: .leading, spacing: Grid.s) {
            Button {
                download(chosen)
            } label: {
                Label(engine.partials[chosen.id].map { ByteText.resume(received: $0, total: chosen.bytes) }
                    ?? "Download, \(ByteText.gb(chosen.bytes))", systemImage: "arrow.down")
            }
            .buttonStyle(.appPrimary)
            .disabled(!engine.serverUp || !checks[chosen.id].mayDownload)
            Text("It resumes where it stopped if the connection drops.").font(.ui(13)).foregroundStyle(Palette.muted)
        }
        if case let .failed(why) = engine.downloads[chosen.id]?.state {
            Banner(text: "The download stopped. Check the connection and the free space, then resume: \(why)", color: Palette.err) {
                Button("Dismiss") { engine.dismissDownload(chosen.id) }.buttonStyle(.appSecondary)
            }
        }
    }

    private func choice(_ e: CatalogEntry, selected: Bool) -> some View {
        Button { pick = e.id } label: {
            VStack(alignment: .leading, spacing: 4) {
                Text(e.title).font(.ui(16, bold: true)).foregroundStyle(Palette.text).fixedSize(horizontal: false, vertical: true)
                Text(ByteText.gb(e.bytes)).font(.ui(13)).foregroundStyle(Palette.muted)
                if case .done = checks[e.id] {
                    Text("Checked").font(.ui(13, weight: .semibold)).foregroundStyle(checks[e.id]?.tone.color ?? Palette.muted)
                }
            }
            .multilineTextAlignment(.leading)
            .padding(12)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .background(RoundedRectangle(cornerRadius: Grid.control, style: .continuous).fill(selected ? Palette.accentTint : Palette.surface))
            .overlay(RoundedRectangle(cornerRadius: Grid.control, style: .continuous)
                .strokeBorder(selected ? Palette.accent : Palette.line, lineWidth: selected ? 2 : Grid.line))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? [.isSelected] : [])
    }

    /// Asks the server once for an entry, when the engine can answer.
    private func runCheck(_ id: String) async {
        guard engine.serverUp, checks[id] == nil else { return }
        checks[id] = .pending
        do {
            checks[id] = .done(try await engine.inspectEntry(id))
        } catch {
            checks[id] = .failed(error.localizedDescription)
        }
    }

    /// The server's answer for the pick, in its words and its tone; how it was checked opens on a tap.
    @ViewBuilder
    private func checkPanel(_ e: CatalogEntry) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            switch checks[e.id] {
            case .none, .pending:
                HStack(spacing: Grid.s) {
                    ProgressView()
                    Text("Checking \(e.title) on this device…").font(.ui(14)).foregroundStyle(Palette.muted)
                }
            case let .failed(why):
                Text("Could not check \(e.title) from here. It can still be downloaded.")
                    .font(.ui(15, weight: .semibold)).foregroundStyle(Palette.text).fixedSize(horizontal: false, vertical: true)
                if !why.isEmpty { Text(why).font(.ui(13)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true) }
            case let .done(c):
                Text("\(e.title): \(c.sentence)").font(.ui(15, weight: .semibold))
                    .foregroundStyle(checks[e.id]?.tone.color ?? Palette.text).fixedSize(horizontal: false, vertical: true)
                if !c.disk.isEmpty {
                    Text(c.disk).font(.ui(13)).foregroundStyle(c.downloadable ? Palette.muted : Palette.err)
                        .fixedSize(horizontal: false, vertical: true)
                }
                DisclosureGroup("How it was checked") {
                    VStack(alignment: .leading, spacing: 4) {
                        if !c.headline.isEmpty { Text(c.headline).font(.ui(13)).foregroundStyle(Palette.text) }
                        if !c.memory.isEmpty { Text(c.memory).font(.ui(13)).foregroundStyle(Palette.muted) }
                    }
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .font(.ui(13))
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .overlay(RoundedRectangle(cornerRadius: Grid.control, style: .continuous).strokeBorder(Palette.line, lineWidth: Grid.line))
    }

    @ViewBuilder
    private func downloadStep(_ e: CatalogEntry) -> some View {
        let p = engine.downloads[e.id]
        let total = (p?.total ?? 0) > 0 ? p!.total : e.bytes
        let got = p?.received ?? 0
        Text("Downloading \(e.title)").font(.ui(18, bold: true)).foregroundStyle(Palette.text).accessibilityAddTraits(.isHeader)
        Meter(fraction: total > 0 ? Double(got) / Double(total) : 0, height: 10)
        if p?.checking == true {
            Text("Checking the file against the catalog's SHA-256…").font(.ui(14)).foregroundStyle(Palette.muted)
        } else {
            Text("\(ByteText.gb(got)) of \(ByteText.gb(total)) (\(total > 0 ? Int((100 * Double(got) / Double(total)).rounded()) : 0)%)")
                .font(.figure(14)).foregroundStyle(Palette.text)
            if let left = ETA.secondsLeft(samples, total: total) {
                (Text("\(ETA.human(left)) left").font(.ui(15, bold: true)).foregroundColor(Palette.text)
                    + Text(ETA.rate(samples).map { " · \(ByteText.gb(Int64($0)))/s" } ?? "").font(.ui(15)).foregroundColor(Palette.muted))
            } else {
                Text("Measuring the speed…").font(.ui(15)).foregroundStyle(Palette.muted)
            }
        }
        Text("You can use the phone meanwhile; the model loads by itself when the download ends.")
            .font(.ui(13)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
        Button("Pause") { engine.cancelDownload(e.id) }.buttonStyle(.appSecondary)
    }

    @ViewBuilder
    private func pausedStep(_ e: CatalogEntry) -> some View {
        let got = row?.partialBytes ?? engine.partials[e.id] ?? 0
        Text("Download of \(e.title) paused").font(.ui(18, bold: true)).foregroundStyle(Palette.text).accessibilityAddTraits(.isHeader)
        Meter(fraction: e.bytes > 0 ? min(1, Double(got) / Double(e.bytes)) : 0, height: 10)
        Text("What is already here stays; it goes on from there.")
            .font(.ui(13)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
        HStack(spacing: Grid.s) {
            Button { download(e) } label: {
                Label(ByteText.resume(received: got, total: e.bytes), systemImage: "arrow.down")
            }
            .buttonStyle(.appPrimary)
            .disabled(!engine.serverUp)
            Button("Pick another model") { repick = true }.buttonStyle(.appSecondary)
        }
        if case let .failed(why) = engine.downloads[e.id]?.state {
            Banner(text: "The download stopped. Check the connection and the free space, then resume: \(why)", color: Palette.err) {
                Button("Dismiss") { engine.dismissDownload(e.id) }.buttonStyle(.appSecondary)
            }
        }
    }

    @ViewBuilder
    private func startStep(_ e: CatalogEntry) -> some View {
        Text("Getting \(e.title) ready").font(.ui(18, bold: true)).foregroundStyle(Palette.text).accessibilityAddTraits(.isHeader)
        Text("The engine measures this phone once, then loads the model. The conversation opens by itself.")
            .font(.ui(15)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
        if let why = loadError {
            Banner(text: why, color: Palette.err)
            Button("Try again") {
                actionError = nil
                started = false
                startIfReady()
            }
            .buttonStyle(.appPrimary)
        } else {
            Skeleton(still: reduceMotion)
        }
    }

    /// Why the load did not happen: the request's refusal, or the engine's error after it.
    private var loadError: String? {
        if let actionError { return actionError }
        if started, !engine.loading, engine.info?.state == .error, let why = engine.info?.error, !why.isEmpty { return why }
        return nil
    }

    // MARK: under the sheet

    private var links: some View {
        VStack(spacing: Grid.s) {
            // The pick step offers these two itself, as buttons.
            if step != 1 || chosen == nil {
                Button("Already have a .gguf? Import a file", action: importFile)
                Button("More models, or one from Hugging Face") { nav.setupDismissed = true }
            }
            Button("Skip for now") {
                nav.setupDismissed = true
                nav.open(.chat)
            }
        }
        .font(.ui(14, weight: .medium))
        .foregroundStyle(Palette.accentInk)
        .buttonStyle(.borderless)
        .multilineTextAlignment(.center)
    }

    // MARK: what happens by itself

    private func download(_ e: CatalogEntry) {
        actionError = nil
        repick = false
        nav.setupEntry = e.id
        pick = e.id
        do { try engine.download(e) } catch { actionError = error.localizedDescription }
    }

    /// The download's pace, for the time left: a sample per progress report while it runs.
    private func sample(_ got: Int64?) {
        guard step == 2, let got, got > 0 else {
            if step != 2, !samples.isEmpty { samples = [] }
            return
        }
        samples = ETA.add(samples, .init(at: ProcessInfo.processInfo.systemUptime, received: got))
    }

    /// Step 3 starts by itself: the downloaded model is loaded, once per visit, never over a load
    /// already running.
    private func startIfReady() {
        guard step == 3, !started, !engine.loading, actionError == nil, row?.state == .onDevice, let path = row?.local?.path
        else { return }
        started = true
        nav.setupEntry = nav.setupEntry ?? chosen?.id
        Task {
            do {
                try await engine.load(path)
            } catch {
                started = false
                actionError = error.localizedDescription
            }
        }
    }
}

/// Where the person is on the path: three numbered steps, the current one a filled ink circle, the
/// done ones a green check, the later ones an outlined number.
struct StepsIndicator: View {
    let current: Int
    private let labels = ["Pick a model", "Download", "Start"]

    var body: some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: Grid.m) { items }
            VStack(alignment: .leading, spacing: Grid.s) { items }
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Setting up: step \(current) of 3, \(labels[max(0, min(2, current - 1))])")
    }

    private var items: some View {
        ForEach(labels.indices, id: \.self) { i in
            let n = i + 1
            HStack(spacing: 6) {
                ZStack {
                    if n < current {
                        Circle().fill(Palette.ok)
                        Image(systemName: "checkmark").font(.system(size: 11, weight: .bold)).foregroundStyle(Palette.surface)
                    } else if n == current {
                        Circle().fill(Palette.primary)
                        Text("\(n)").font(.ui(13, bold: true)).foregroundStyle(Palette.onPrimary)
                    } else {
                        Circle().strokeBorder(Palette.lineStrong, lineWidth: Grid.line)
                        Text("\(n)").font(.ui(13)).foregroundStyle(Palette.muted)
                    }
                }
                .frame(width: 24, height: 24)
                Text(labels[i]).font(.ui(14, weight: n == current ? .semibold : .regular))
                    .foregroundStyle(n == current ? Palette.text : Palette.muted)
            }
        }
    }
}

/// A bar standing in for what is on its way, breathing gently unless motion is reduced.
struct Skeleton: View {
    var still = false

    var body: some View {
        TimelineView(.animation(paused: still)) { tl in
            let phase = tl.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 2.4) / 2.4
            Capsule().fill(Palette.surface2)
                .opacity(still ? 1 : 0.55 + 0.45 * (0.5 + 0.5 * cos(2 * .pi * phase)))
        }
        .frame(height: 10)
        .accessibilityHidden(true)
    }
}
