import BmoeKit
import SwiftUI
import UniformTypeIdentifiers

/// Models (docs/screens.md): what is on this device, how to get a model onto it, and which one is
/// loaded. One list, "Your models", where the files the server lists and the catalog's entries are
/// merged by file (`Library.rows`), each row in one state with one primary action; a guided way to
/// add to it; and nothing else. The server says what every file is; the app moves the bytes.
struct ModelsView: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var nav: Navigator
    @State private var failure: String?
    @State private var importing = false
    @State private var busy: Set<String> = []
    @State private var confirmDelete: PendingDelete?
    /// The entry the person just started: its row is scrolled to, now and again when it lands.
    @State private var followed: String?

    /// A delete waiting for its second press.
    struct PendingDelete: Identifiable {
        let name: String
        let title: String
        let files: Int
        let bytes: Int64
        var id: String { name }
    }

    private var split: (models: [LocalModel], companions: [LocalModel]) {
        // Projectors and draft heads (by their headers) are no model to load: listed apart, after the models.
        FileRole.split(engine.models.local) { $0.role }
    }

    private var rows: [Library.Row] {
        Library.rows(engine.models, local: split.models, downloading: engine.downloading, partials: engine.partials,
                     loadedPath: engine.info?.model?.path)
    }

    var body: some View {
        let rows = self.rows
        let yours = rows.filter { !$0.suggested }
        let suggested = rows.filter(\.suggested)
        let companions = split.companions
        ScrollViewReader { proxy in
            Page {
                if let failure {
                    Banner(text: failure, color: Palette.err) {
                        Button("Dismiss") { self.failure = nil }.buttonStyle(.pixel)
                    }
                }
                if engine.info?.state == .error, let why = engine.info?.error, !why.isEmpty {
                    Banner(text: "The last load failed: \(why)", color: Palette.err)
                }
                LoadStages()
                loadedElsewhere(rows)
                if !engine.modelsListed {
                    Text(engine.serverUp ? "Reading the models…" : "The engine is not running.")
                        .font(.subheadline).foregroundStyle(Palette.muted)
                } else if yours.isEmpty, companions.isEmpty {
                    firstModel(suggested)
                } else {
                    SectionTitle("Your models", trailing: "\(yours.count)")
                    ForEach(yours) { r in row(r).id(r.id) }
                    if !companions.isEmpty { companionFold(companions) }
                    if !suggested.isEmpty {
                        SectionTitle("Suggested")
                        ForEach(suggested) { r in row(r).id(r.id) }
                    }
                    SectionTitle("Get another model")
                    HuggingFaceSection(onAdded: follow)
                    otherModel
                }
                whereModelsLive
            }
            .onChange(of: followed) { _ in scroll(proxy, rows) }
            // A download that lands moves from the catalog's row to the file's: follow it there.
            .onChange(of: rows.map(\.id)) { _ in scroll(proxy, self.rows) }
        }
        .refreshable { await engine.refreshModels() }
        .task { await engine.refreshModels() }
        .fileImporter(isPresented: $importing, allowedContentTypes: [UTType(filenameExtension: "gguf") ?? .data, .data]) { result in
            guard case let .success(url) = result else { return }
            run("import") { try await engine.importModel(from: url) }
        }
        .confirmationDialog("Delete this model?", isPresented: Binding(get: { confirmDelete != nil }, set: { if !$0 { confirmDelete = nil } }),
                            titleVisibility: .visible, presenting: confirmDelete)
        { d in
            Button("Delete \(d.files > 1 ? "all \(d.files) files" : d.title)", role: .destructive) {
                run("del:\(d.name)") { try await engine.delete(name: d.name) }
            }
            Button("Keep", role: .cancel) {}
        } message: { d in
            Text("\(ByteText.gb(d.bytes)) is removed from this device.")
        }
    }

    private func follow(_ id: String) { followed = id }

    private func scroll(_ proxy: ScrollViewProxy, _ rows: [Library.Row]) {
        guard let followed, let r = rows.first(where: { $0.entry?.id == followed }) else { return }
        withAnimation { proxy.scrollTo(r.id, anchor: .center) }
    }

    /// A model opened from outside the listed folders has no row to carry Unload and Go to Chat.
    @ViewBuilder
    private func loadedElsewhere(_ rows: [Library.Row]) -> some View {
        if let m = engine.info?.model, !rows.contains(where: { $0.state == .loaded }) {
            Banner(text: "Loaded: \(m.name)", color: Palette.ok) {
                HStack(spacing: Grid.s) {
                    Button("Unload") { run("unload") { try await engine.unload() } }.buttonStyle(.pixel)
                    Button("Go to Chat") { nav.open(.chat) }.buttonStyle(.pixelPrimary)
                }
            }
        }
    }

    // MARK: nothing here yet

    /// With nothing on the device and nothing coming: the three steps to a first model.
    private func firstModel(_ suggested: [Library.Row]) -> some View {
        VStack(alignment: .leading, spacing: Grid.m) {
            Text("Get your first model").font(.pixel(22, bold: true)).foregroundStyle(Palette.text)
            VStack(alignment: .leading, spacing: 6) {
                step(1, "Find a model", "search by name, or paste a repository.")
                step(2, "Check it fits here", "the server reads its header and sets it against this device, before any download.")
                step(3, "Download, then Load", "Load appears on its row in Your models.")
            }
            HuggingFaceSection(onAdded: follow)
            if !suggested.isEmpty {
                Text("Or start from a suggestion").font(.pixel(16, bold: true)).foregroundStyle(Palette.text)
                ForEach(suggested) { r in
                    if let e = r.entry {
                        Button("Download \(e.title)\(e.quant.isEmpty ? "" : " (\(e.quant))"), \(ByteText.gb(e.bytes))") {
                            download(e)
                        }
                        .buttonStyle(.pixel)
                        // A suggestion whose download failed before a byte landed is still one: its
                        // failure is said beside the button that started it.
                        if case let .failed(why) = engine.downloads[e.id]?.state {
                            Banner(text: "\(e.title): \(why)", color: Palette.err) {
                                Button("Dismiss") { engine.dismissDownload(e.id) }.buttonStyle(.pixel)
                            }
                        }
                    }
                }
            }
            otherModel
        }
    }

    private func step(_ n: Int, _ what: String, _ how: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: Grid.s) {
            Text("\(n)").font(.pixel(16, bold: true)).foregroundStyle(Palette.accent)
            (Text(what).bold() + Text(": " + how)).font(.subheadline).foregroundStyle(Palette.text)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    // MARK: a row

    private func stateText(_ s: Library.RowState) -> (String, Color) {
        switch s {
        case .available: return ("Available", Palette.muted)
        case .downloading: return ("Downloading", Palette.accent)
        case .partial: return ("Partly downloaded", Palette.warn)
        case .onDevice: return ("On this device", Palette.ok)
        case .loaded: return ("Loaded", Palette.ok)
        case .incomplete: return ("Shards missing", Palette.err)
        }
    }

    private func row(_ r: Library.Row) -> some View {
        let m = r.local, e = r.entry
        let progress = e.flatMap { engine.downloads[$0.id] }
        let (label, colour) = stateText(r.state)
        let meta = metaLine(r)
        return VStack(alignment: .leading, spacing: Grid.s) {
            Text(r.title).font(.headline).foregroundStyle(Palette.text).lineLimit(2)
            HStack(spacing: 6) {
                if let q = e?.quant, !q.isEmpty { Badge(text: q) }
                Badge(text: label, color: colour)
                if let m {
                    Badge(text: m.streamable ? "streams" : "plain mmap", color: m.streamable ? Palette.accent : Palette.warn)
                }
                if e?.added == true { Badge(text: "added") }
            }
            Text(meta).font(.pixel(13)).foregroundStyle(Palette.muted).lineLimit(3)
            if let blurb = e?.blurb, !blurb.isEmpty, m == nil { Text(blurb).font(.subheadline).foregroundStyle(Palette.muted) }
            if r.state == .downloading, let p = progress {
                Meter(fraction: p.fraction)
                Text(p.checking ? "Checking the file against the catalog's SHA-256…"
                    : p.total > 0 ? "\(ByteText.gb(p.received)) of \(ByteText.gb(p.total))" : ByteText.gb(p.received))
                    .font(.pixel(13)).foregroundStyle(Palette.muted)
            } else if r.state == .partial, let got = r.partialBytes, let e, e.bytes > 0 {
                // Stopped by a failure, a cancel or a relaunch: what is on the device already.
                Meter(fraction: min(1, Double(got) / Double(e.bytes)))
            }
            if let e, case let .failed(why) = progress?.state {
                Banner(text: why, color: Palette.err) {
                    Button("Dismiss") { engine.dismissDownload(e.id) }.buttonStyle(.pixel)
                }
            }
            actions(r)
        }
        .card()
        .overlay(Rectangle().strokeBorder(e != nil && e?.id == followed ? Palette.accent : .clear, lineWidth: Grid.edge))
    }

    /// Size, architecture, shards, the file's own name where the title is another, and where it sits.
    private func metaLine(_ r: Library.Row) -> String {
        var parts: [String] = []
        if let m = r.local {
            parts = [ByteText.gb(m.bytes), m.arch, m.shards > 1 ? "\(m.shards) shards" : "", m.name != r.title ? m.name : "",
                     m.folder.isEmpty ? "" : "in \(m.folder)", m.dir != engine.models.modelsDir ? "in \(m.dir)" : ""]
        } else if let e = r.entry {
            parts = [ByteText.gb(e.bytes), e.note, e.file]
        }
        return parts.filter { !$0.isEmpty }.joined(separator: " · ")
    }

    @ViewBuilder
    private func actions(_ r: Library.Row) -> some View {
        let m = r.local, e = r.entry
        // Wraps on a narrow screen rather than squeezing the labels.
        VStack(alignment: .leading, spacing: Grid.s) {
            HStack(spacing: Grid.s) {
                switch r.state {
                case .loaded:
                    if let m {
                        Button("Reload") { run("load:\(m.path)") { try await engine.load(m.path) } }.buttonStyle(.pixel)
                            .disabled(engine.loading)
                    }
                    Button("Unload") { run("unload") { try await engine.unload() } }.buttonStyle(.pixel)
                    Button("Go to Chat") { nav.open(.chat) }.buttonStyle(.pixelPrimary)
                case .onDevice:
                    if let m {
                        Button("Load") { run("load:\(m.path)") { try await engine.load(m.path) } }
                            .buttonStyle(.pixelPrimary)
                            .disabled(engine.loading || !engine.serverUp || busy.contains("load:\(m.path)"))
                    }
                case .downloading:
                    if let e { Button("Cancel") { engine.cancelDownload(e.id) }.buttonStyle(.pixelDestructive) }
                case .partial, .available, .incomplete:
                    if let e {
                        Button(r.partialBytes.map { ByteText.resume(received: $0, total: e.bytes) } ?? "Download") { download(e) }
                            .buttonStyle(.pixelPrimary)
                    }
                }
            }
            HStack(spacing: Grid.s) {
                if r.partialBytes != nil, r.state == .partial, let e {
                    // The one action that throws the bytes away: Cancel keeps them.
                    Button("Delete partial") { run("part:\(e.id)") { try await engine.deletePartial(e) } }
                        .buttonStyle(.pixelDestructive)
                }
                if let name = r.deleteName, let m, r.state != .loaded, r.state != .downloading {
                    Button("Delete…") {
                        confirmDelete = PendingDelete(name: name, title: r.title, files: m.files.count, bytes: m.bytes)
                    }
                    .buttonStyle(.pixelDestructive)
                }
                if let e, e.added, r.state != .downloading {
                    // Off the list; the files stay.
                    Button("Forget") { run("rm:\(e.id)") { try await engine.removeAdded(e.id) } }.buttonStyle(.pixel)
                }
            }
        }
        .disabled(!busy.isEmpty)
    }

    private func download(_ e: CatalogEntry) {
        followed = e.id
        engine.download(e)
    }

    // MARK: companions, other models, where they live

    private func companionFold(_ companions: [LocalModel]) -> some View {
        Fold(title: "\(companions.count) companion \(companions.count == 1 ? "file" : "files")",
             hint: "Not models to load, by their headers: projectors and draft heads go with a model.")
        {
            ForEach(companions) { m in
                VStack(alignment: .leading, spacing: 6) {
                    HStack(alignment: .firstTextBaseline) {
                        Text(m.name).font(.subheadline.weight(.semibold)).foregroundStyle(Palette.text).lineLimit(2)
                        Spacer()
                        Badge(text: FileRole.label(m.role))
                    }
                    Text([ByteText.gb(m.bytes), m.arch, m.folder.isEmpty ? "" : "in \(m.folder)"].filter { !$0.isEmpty }
                        .joined(separator: " · "))
                        .font(.pixel(13)).foregroundStyle(Palette.muted)
                    if let name = Library.deleteName(m, modelsDir: engine.models.modelsDir) {
                        Button("Delete…") { confirmDelete = PendingDelete(name: name, title: m.name, files: m.files.count, bytes: m.bytes) }
                            .buttonStyle(.pixelDestructive).disabled(!busy.isEmpty)
                    }
                }
                .padding(.vertical, 6)
            }
        }
    }

    private var otherModel: some View {
        VStack(alignment: .leading, spacing: Grid.m) {
            Text("Any other model").font(.pixel(16, bold: true)).foregroundStyle(Palette.text)
            UrlImportSection(onAdded: follow)
            Text("Or a .gguf already on this device, in Files or iCloud Drive, copied into the app's folder.")
                .font(.subheadline).foregroundStyle(Palette.muted)
            Button("Import a file") { importing = true }.buttonStyle(.pixel).disabled(!busy.isEmpty)
        }
        .card()
    }

    private var whereModelsLive: some View {
        let list = engine.models
        return Fold(title: "Where models live") {
            VStack(alignment: .leading, spacing: Grid.s) {
                Text("Downloads land in the app's folder, which the Files app and the Finder show; a .gguf copied there "
                    + "(or into a folder directly inside it) is listed too.")
                    .font(.caption).foregroundStyle(Palette.muted)
                if !list.modelsDir.isEmpty { Text(list.modelsDir).font(.code).foregroundStyle(Palette.text).textSelection(.enabled) }
                if !list.scanDirs.isEmpty {
                    Text("Also listed, and loaded in place:").font(.caption).foregroundStyle(Palette.muted)
                    ForEach(list.scanDirs, id: \.self) { Text($0).font(.code).foregroundStyle(Palette.text).textSelection(.enabled) }
                }
            }
        }
    }

    private func run(_ key: String, _ action: @escaping () async throws -> Void) {
        busy.insert(key)
        Task {
            do { try await action() } catch { failure = error.localizedDescription }
            busy.remove(key)
        }
    }
}

/// Any other model: a direct https link to a gguf, or to any shard of a split set. The server
/// checks the link, asks the sizes and makes it a catalog entry (`POST /api/models/url`); the app
/// then downloads the entry's files like any other. Nothing here reads the link beyond trimming it.
struct UrlImportSection: View {
    @EnvironmentObject private var engine: Engine
    var onAdded: (String) -> Void
    @State private var url = ""
    @State private var working = false
    @State private var failure: String?

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            PixelField(placeholder: "https://.../model.gguf", text: $url, mono: true)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .keyboardType(.URL)
                .onSubmit(add)
            HStack(spacing: Grid.s) {
                Button(working ? "Checking the link…" : "Download", action: add).buttonStyle(.pixelPrimary)
                    .disabled(url.trimmingCharacters(in: .whitespaces).isEmpty || working || !engine.serverUp)
                if working { ProgressView().tint(Palette.accent) }
            }
            Text("A link to one file, or to any shard of a split set (the other shards are fetched beside it). It is "
                + "added to Your models and downloads like any other.")
                .font(.caption).foregroundStyle(Palette.muted)
            if let failure { Banner(text: failure, color: Palette.err) }
        }
    }

    private func add() {
        working = true
        failure = nil
        Task {
            do {
                let id = try await engine.addURL(url)
                url = ""
                onAdded(id)
            } catch {
                failure = error.localizedDescription
            }
            working = false
        }
    }
}

/// From Hugging Face (docs/screens.md, Models), as four steps: Find (a name searched, or a
/// repository listed), Pick (one of its models), Check (the server reads its header against this
/// device), Download (it becomes a catalog entry and its download starts). The verdict's headline
/// and its colour are the server's (`verdict.headline`, `verdict.fit`); nothing here keeps a threshold.
struct HuggingFaceSection: View {
    @EnvironmentObject private var engine: Engine
    var onAdded: (String) -> Void
    @State private var query = ""
    @State private var token = ""
    @State private var showToken = false
    @State private var hits: HFSearch?
    @State private var listing: HFModels?
    @State private var selected = ""
    @State private var facts: HFFacts?
    @State private var added = ""
    @State private var working: String?
    @State private var failure: String?
    @State private var companionsOpen = false

    static let steps = ["Find", "Pick", "Check", "Download"]

    /// Where the person is: what they have, not what they last pressed.
    private var step: Int {
        let chosen = listing?.models.contains { $0.name == selected } == true
        if !added.isEmpty || (facts != nil && chosen) { return 3 }
        if chosen { return 2 }
        return listing != nil || hits != nil ? 1 : 0
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.m) {
            Text("From Hugging Face").font(.pixel(16, bold: true)).foregroundStyle(Palette.text)
            Steps(labels: Self.steps, current: step)
            Text("A model's name to search for, or a repository id or URL. The server lists the repository's gguf files, "
                + "reads the header of the one you pick before any download, and says whether it fits here.")
                .font(.caption).foregroundStyle(Palette.muted)
            PixelField(placeholder: "A model's name, owner/repo, or its URL", text: $query, mono: true)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .onSubmit(find)
            HStack(spacing: Grid.s) {
                Button(working == "find" ? "Looking…" : "Find", action: find).buttonStyle(.pixelPrimary)
                    .disabled(query.trimmingCharacters(in: .whitespaces).isEmpty || working != nil || !engine.serverUp)
                Button(showToken ? "Hide token" : "Token") { showToken.toggle() }.buttonStyle(.pixel)
                if working != nil { ProgressView().tint(Palette.accent) }
            }
            if showToken {
                SecureField("Token for a gated or private repository", text: $token)
                    .font(.code)
                    .padding(10)
                    .pixelBox(shadow: false)
                Text("Used for these requests and kept nowhere.").font(.caption).foregroundStyle(Palette.muted)
            }
            if let failure { Banner(text: failure, color: Palette.err) }
            if let hits, listing == nil { repos(hits) }
            if let listing { models(listing) }
            if let facts, let l = listing, let m = l.models.first(where: { $0.name == selected }) { verdict(facts, m) }
        }
        .card()
    }

    private func repos(_ s: HFSearch) -> some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Text(s.repos.isEmpty ? "No repository with gguf files matches." : "Repositories, the most downloaded first")
                .font(.pixel(13)).foregroundStyle(Palette.muted)
            ForEach(s.repos) { r in
                Button { open(r.id) } label: {
                    HStack {
                        Text(r.id).font(.code).foregroundStyle(Palette.text).lineLimit(1)
                        Spacer()
                        if r.gated { Badge(text: "gated", color: Palette.warn) }
                        let updated = FileRole.isoDate(r.modified)
                        Text(updated.isEmpty ? "\(r.downloads)↓" : "\(r.downloads)↓ · \(updated)").font(.pixel(13)).foregroundStyle(Palette.muted)
                    }
                    .padding(10)
                    .pixelBox(shadow: false)
                }
            }
        }
    }

    private func models(_ l: HFModels) -> some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            HStack {
                Text(l.repo).font(.code).foregroundStyle(Palette.text)
                Spacer()
                if hits != nil { Button("Back") { listing = nil; facts = nil; selected = "" }.buttonStyle(.pixel) }
            }
            if l.models.isEmpty { Text("This repository holds no gguf.").font(.subheadline).foregroundStyle(Palette.muted) }
            // Files whose names suggest a projector or a draft head go after the models, folded: the
            // name is only a hint, so they can still be checked, and the header then decides.
            let parts = FileRole.split(l.models) { $0.role }
            let companions = parts.companions
            ForEach(parts.models) { m in choice(m) }
            if !companions.isEmpty {
                Button {
                    withAnimation { companionsOpen.toggle() }
                } label: {
                    HStack(alignment: .firstTextBaseline) {
                        Text("\(companions.count) companion \(companions.count == 1 ? "file" : "files") (projectors, draft heads, "
                            + "going by their names; Check reads the header)")
                            .font(.caption).foregroundStyle(Palette.muted).multilineTextAlignment(.leading)
                        Spacer()
                        Image(systemName: companionsOpen ? "chevron.up" : "chevron.down").foregroundStyle(Palette.muted)
                    }
                }
                if companionsOpen { ForEach(companions) { m in choice(m) } }
            }
            if !l.models.isEmpty {
                HStack(spacing: Grid.s) {
                    Button(working == "check" ? "Reading the header…" : "Check it fits here") { check(l.repo) }
                        .buttonStyle(.pixelPrimary)
                        .disabled(selected.isEmpty || working != nil)
                    if selected.isEmpty { Text("Pick a model above first.").font(.caption).foregroundStyle(Palette.muted) }
                }
            }
        }
    }

    /// One model of the repository, picked with a tap.
    private func choice(_ m: HFModel) -> some View {
        let picked = m.name == selected
        return Button {
            selected = m.name
            facts = nil
            added = ""
        } label: {
            HStack(alignment: .firstTextBaseline, spacing: Grid.s) {
                Image(systemName: picked ? "largecircle.fill.circle" : "circle").foregroundStyle(picked ? Palette.accent : Palette.muted)
                VStack(alignment: .leading, spacing: 2) {
                    Text(m.name).font(.subheadline.weight(.semibold)).foregroundStyle(Palette.text).lineLimit(2)
                        .multilineTextAlignment(.leading)
                    HStack(spacing: 6) {
                        Text([m.quant, ByteText.gb(m.bytes), m.count > 1 ? "\(m.count) shards" : ""].filter { !$0.isEmpty }
                            .joined(separator: " · "))
                            .font(.pixel(13)).foregroundStyle(Palette.muted)
                        if !m.complete { Badge(text: "incomplete", color: Palette.err) }
                    }
                }
                Spacer(minLength: 0)
            }
            .padding(10)
            .background(picked ? Palette.tint(Palette.accent) : .clear)
            .overlay(Rectangle().strokeBorder(picked ? Palette.accent : Palette.border, lineWidth: 1))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    /// The server's verdict first, coloured by its code, then the facts it read, then Download.
    private func verdict(_ f: HFFacts, _ m: HFModel) -> some View {
        let tone = f.tone.color
        return VStack(alignment: .leading, spacing: Grid.s) {
            Text(f.headline.isEmpty ? f.mode : f.headline).font(.pixel(18, bold: true)).foregroundStyle(tone ?? Palette.text)
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: Grid.s) {
                Badge(text: f.mode, color: tone ?? Palette.muted)
                if FileRole.isCompanion(f.role) { Badge(text: FileRole.label(f.role)) }
            }
            if f.readable {
                fact("Architecture", f.arch.isEmpty ? "?" : f.arch)
                fact("Experts", f.isMoE ? "\(f.nExpert), \(f.nExpertUsed) per token" : "none (dense)")
                if f.streamable {
                    fact("Layers", "\(f.nLayer) (\(f.nMoeLayer) with experts)")
                    fact("Resident", Bytes.label(f.denseBytes))
                    fact("Experts on flash", "\(Bytes.label(f.expertBytes)), \(Bytes.label(f.tokenCycleBytes)) a token")
                }
                fact("On disk", Bytes.label(f.bytes) + (f.files > 1 ? " in \(f.files) files" : ""))
            }
            if !f.refusal.isEmpty { Text(f.refusal).font(.caption).foregroundStyle(Palette.warn) }
            if !f.memory.isEmpty { Text(f.memory).font(.caption).foregroundStyle(Palette.muted) }
            if !f.disk.isEmpty { Text(f.disk).font(.caption).foregroundStyle(f.downloadable ? Palette.muted : Palette.err) }
            if !added.isEmpty {
                HStack(spacing: Grid.s) {
                    Badge(text: "Downloading", color: Palette.ok)
                    Text("Follow it in Your models; Load appears on its row when it is done.").font(.caption).foregroundStyle(Palette.muted)
                }
            } else {
                Button(working == "add" ? "Starting…" : "Download") { add(listing?.repo ?? "", m.name) }
                    .buttonStyle(.pixelPrimary)
                    .disabled(!f.downloadable || !m.complete || working != nil)
                if !m.complete {
                    Text("Shards are missing from the repository: it cannot be downloaded whole.").font(.caption).foregroundStyle(Palette.muted)
                }
            }
        }
        .padding(12)
        .pixelBox(fill: Palette.bg, edge: tone ?? Palette.border, shadow: false)
        .fixedSize(horizontal: false, vertical: true)
    }

    private func fact(_ k: String, _ v: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(k).font(.caption).foregroundStyle(Palette.muted)
            Spacer()
            Text(v).font(.pixel(13)).foregroundStyle(Palette.text).multilineTextAlignment(.trailing)
        }
    }

    private func find() {
        hits = nil
        open(query)
    }

    /// A repository is listed; text that is not one is searched for.
    private func open(_ text: String) {
        work("find") {
            facts = nil
            added = ""
            do {
                let l = try await engine.hfModels(text, token: token)
                listing = l
                companionsOpen = false
                // A URL that named a file preselects it; a repository with one model needs no choice.
                selected = !l.file.isEmpty && l.models.contains { $0.name == l.file } ? l.file : l.models.count == 1 ? l.models[0].name : ""
            } catch let e as APIError where e.code == APIError.notARepo {
                listing = nil
                selected = ""
                hits = try await engine.hfSearch(text, token: token)
            }
        }
    }

    private func check(_ repo: String) {
        let name = selected
        work("check") { facts = try await engine.hfInspect(repo, name: name, token: token) }
    }

    private func add(_ repo: String, _ name: String) {
        // "Downloading" only once the download has started: a refusal shows here instead.
        work("add") {
            let id = try await engine.hfAdd(repo, name: name, token: token)
            added = id
            onAdded(id)
        }
    }

    private func work(_ what: String, _ action: @escaping () async throws -> Void) {
        working = what
        failure = nil
        Task {
            do { try await action() } catch { failure = error.localizedDescription }
            working = nil
        }
    }
}

extension FitTone {
    /// The verdict's colour; none for a code this app does not know.
    var color: Color? {
        switch self {
        case .ok: return Palette.ok
        case .warn: return Palette.warn
        case .err: return Palette.err
        case .none: return nil
        }
    }
}
