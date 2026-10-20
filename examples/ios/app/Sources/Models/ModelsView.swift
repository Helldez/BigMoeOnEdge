import BmoeKit
import SwiftUI
import UniformTypeIdentifiers

/// Models (docs/screens.md): what is on this device, how to get a model onto it, and which one is
/// loaded. The server says what every file is; the app moves the bytes.
struct ModelsView: View {
    @EnvironmentObject private var engine: Engine
    @State private var failure: String?
    @State private var importing = false
    @State private var busy = false
    @State private var confirmDelete: LocalModel?

    var body: some View {
        Page {
            if let failure {
                Banner(text: failure, color: Palette.err) {
                    Button("Dismiss") { self.failure = nil }.buttonStyle(.pixel)
                }
            }
            LoadStages()
            onDevice
            catalog
            HuggingFaceSection()
            other
        }
        .refreshable { await engine.refreshModels() }
        .task { await engine.refreshModels() }
        .fileImporter(isPresented: $importing, allowedContentTypes: [UTType(filenameExtension: "gguf") ?? .data, .data]) { result in
            guard case let .success(url) = result else { return }
            run { try await engine.importModel(from: url) }
        }
        .confirmationDialog("Delete this model?", isPresented: Binding(get: { confirmDelete != nil }, set: { if !$0 { confirmDelete = nil } }),
                            titleVisibility: .visible, presenting: confirmDelete)
        { m in
            Button("Delete \(m.files.count > 1 ? "\(m.files.count) files" : m.name)", role: .destructive) {
                run { try await engine.delete(m) }
            }
        } message: { m in
            Text("\(ByteText.gb(m.bytes)) is removed from this device.")
        }
    }

    // MARK: on this device

    private var onDevice: some View {
        // Projectors and draft heads (by their headers) are no model to load: listed apart, after the models.
        let (models, companions) = FileRole.split(engine.models.local) { $0.role }
        return VStack(alignment: .leading, spacing: Grid.m) {
            SectionTitle("On this device", trailing: models.isEmpty ? nil : "\(models.count)")
            if models.isEmpty {
                Text("No model yet. Download one below, add one from Hugging Face, or copy a .gguf into this app's "
                    + "folder with the Files app or the Finder.")
                    .font(.subheadline).foregroundStyle(Palette.muted)
            }
            ForEach(models) { m in localRow(m) }
            if !companions.isEmpty {
                Text("Companion files, not models to load (by their headers)").font(.pixel(13)).foregroundStyle(Palette.muted)
                ForEach(companions) { m in companionRow(m) }
            }
        }
    }

    /// A file that only goes with a model: what it is, and Delete, as any file in the app's folder.
    private func companionRow(_ m: LocalModel) -> some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            HStack(alignment: .firstTextBaseline) {
                Text(m.name).font(.subheadline.weight(.semibold)).foregroundStyle(Palette.text).lineLimit(2)
                Spacer()
                Badge(text: FileRole.label(m.role), color: Palette.muted)
            }
            Text([ByteText.gb(m.bytes), m.arch, m.folder.isEmpty ? "" : "in \(m.folder)"].filter { !$0.isEmpty }.joined(separator: " · "))
                .font(.pixel(13)).foregroundStyle(Palette.muted)
            Button("Delete") { confirmDelete = m }.buttonStyle(.pixelDestructive).disabled(busy)
        }
        .card(padding: 12)
    }

    private func localRow(_ m: LocalModel) -> some View {
        let loaded = engine.info?.model?.path == m.path
        return VStack(alignment: .leading, spacing: Grid.s) {
            HStack(alignment: .firstTextBaseline) {
                Text(m.name).font(.headline).foregroundStyle(Palette.text).lineLimit(2)
                Spacer()
                if loaded { Badge(text: "loaded", color: Palette.ok) }
            }
            HStack(spacing: Grid.s) {
                Text(ByteText.gb(m.bytes)).font(.pixel(14))
                if !m.arch.isEmpty { Text(m.arch).font(.pixel(14)) }
                if m.shards > 1 { Text("\(m.shards) shards").font(.pixel(14)) }
                if !m.folder.isEmpty { Text("in \(m.folder)").font(.pixel(14)).lineLimit(1) }
            }
            .foregroundStyle(Palette.muted)
            HStack(spacing: Grid.s) {
                if !m.complete { Badge(text: "shards missing", color: Palette.warn) }
                Badge(text: m.streamable ? "streams" : "plain mmap", color: m.streamable ? Palette.accent : Palette.muted)
            }
            HStack(spacing: Grid.s) {
                if loaded {
                    Button("Unload") { run { try await engine.unload() } }.buttonStyle(.pixel)
                } else {
                    Button("Load") { run { try await engine.load(m.path) } }
                        .buttonStyle(.pixelPrimary)
                        .disabled(!m.complete || engine.loading || !engine.serverUp)
                    Button("Delete") { confirmDelete = m }.buttonStyle(.pixelDestructive)
                }
            }
            .disabled(busy)
        }
        .card()
    }

    // MARK: the catalog

    private var catalog: some View {
        VStack(alignment: .leading, spacing: Grid.m) {
            SectionTitle("Catalog")
            ForEach(engine.models.catalog) { e in catalogRow(e) }
        }
    }

    private func catalogRow(_ e: CatalogEntry) -> some View {
        let progress = engine.downloads[e.id]
        let status = engine.models.status(of: e, downloading: Set(engine.downloads.filter { $0.value.state == .running }.keys))
        let partial = status == .available ? engine.partials[e.id] : nil
        return VStack(alignment: .leading, spacing: Grid.s) {
            HStack(alignment: .firstTextBaseline) {
                Text(e.title).font(.headline).foregroundStyle(Palette.text)
                Spacer()
                Text([e.quant, ByteText.gb(e.bytes)].filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.pixel(14)).foregroundStyle(Palette.muted)
            }
            if !e.blurb.isEmpty { Text(e.blurb).font(.subheadline).foregroundStyle(Palette.muted) }
            if let p = progress, p.state == .running {
                Meter(fraction: p.fraction)
                Text("\(ByteText.gb(p.received)) of \(ByteText.gb(p.total))").font(.pixel(13)).foregroundStyle(Palette.muted)
            } else if let got = partial, e.bytes > 0 {
                // Stopped by a failure, a cancel or a relaunch: what is on the device already.
                Meter(fraction: min(1, Double(got) / Double(e.bytes)))
            }
            if case let .failed(why) = progress?.state {
                Banner(text: why, color: Palette.err) {
                    Button("Dismiss") { engine.dismissDownload(e.id) }.buttonStyle(.pixel)
                }
            }
            HStack(spacing: Grid.s) {
                switch status {
                case .downloading:
                    Button("Cancel") { engine.cancelDownload(e.id) }.buttonStyle(.pixel)
                case .onDevice:
                    Badge(text: "on device", color: Palette.ok)
                case .available:
                    Button(partial.map { ByteText.resume(received: $0, total: e.bytes) } ?? "Download") { engine.download(e) }
                        .buttonStyle(.pixelPrimary)
                }
                if e.added && status != .downloading {
                    Spacer()
                    Button("Remove") { run { try await engine.removeAdded(e.id) } }.buttonStyle(.pixel)
                }
            }
            if partial != nil {
                // The one action that throws the bytes away: Cancel keeps them.
                Button("Delete partial download") { run { try await engine.deletePartial(e) } }
                    .buttonStyle(.pixelDestructive)
                    .disabled(busy)
            }
        }
        .card()
    }

    // MARK: any other model

    private var other: some View {
        VStack(alignment: .leading, spacing: Grid.m) {
            SectionTitle("Any other model")
            Text("A .gguf already on this device, in Files or iCloud Drive, is copied into the app's folder. One copied in "
                + "with the Finder while the device is attached is listed as it is.")
                .font(.subheadline).foregroundStyle(Palette.muted)
            Button("Import a file") { importing = true }.buttonStyle(.pixel).disabled(busy)
        }
    }

    private func run(_ action: @escaping () async throws -> Void) {
        busy = true
        Task {
            do { try await action() } catch { failure = error.localizedDescription }
            busy = false
        }
    }
}

/// From Hugging Face (docs/screens.md, Models): a model's name, or a repository id or its URL, and
/// the steps the server does: find, check the header before any download, add to the catalog.
struct HuggingFaceSection: View {
    @EnvironmentObject private var engine: Engine
    @State private var query = ""
    @State private var token = ""
    @State private var showToken = false
    @State private var hits: HFSearch?
    @State private var listing: HFModels?
    @State private var facts: [String: HFFacts] = [:]
    @State private var working: String?
    @State private var failure: String?
    @State private var companionsOpen = false

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.m) {
            SectionTitle("From Hugging Face")
            PixelField(placeholder: "A model's name, owner/repo, or its URL", text: $query, mono: true)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .onSubmit(find)
            HStack(spacing: Grid.s) {
                Button("Find", action: find).buttonStyle(.pixelPrimary)
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
        }
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
                if hits != nil { Button("Back") { listing = nil }.buttonStyle(.pixel) }
            }
            if l.models.isEmpty { Text("This repository holds no gguf.").font(.subheadline).foregroundStyle(Palette.muted) }
            // Files whose names suggest a projector or a draft head go after the models, folded: the
            // name is only a hint, so they can still be checked, and the header then decides.
            let parts = FileRole.split(l.models) { $0.role }
            let companions = parts.companions
            ForEach(parts.models) { m in modelChoice(l, m) }
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
                if companionsOpen { ForEach(companions) { m in modelChoice(l, m) } }
            }
        }
    }

    private func modelChoice(_ l: HFModels, _ m: HFModel) -> some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            HStack(alignment: .firstTextBaseline) {
                Text(m.name).font(.subheadline.weight(.semibold)).foregroundStyle(Palette.text).lineLimit(2)
                Spacer()
                Text([m.quant, ByteText.gb(m.bytes)].filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.pixel(13)).foregroundStyle(Palette.muted)
            }
            if m.count > 1 { Text("\(m.count) shards").font(.pixel(13)).foregroundStyle(Palette.muted) }
            if !m.complete { Badge(text: "shards missing", color: Palette.warn) }
            if let f = facts[m.name] { FactsView(facts: f) }
            HStack(spacing: Grid.s) {
                Button("Check") { check(l.repo, m.name) }.buttonStyle(.pixel).disabled(working != nil)
                Button("Add") { add(l.repo, m.name) }.buttonStyle(.pixelPrimary)
                    .disabled(working != nil || !m.complete || facts[m.name]?.downloadable == false)
            }
        }
        .card(padding: 12)
        .background(m.name == l.file ? Palette.tint(Palette.accent) : .clear)
    }

    private func find() {
        hits = nil
        open(query)
    }

    /// A repository is listed; text that is not one is searched for.
    private func open(_ text: String) {
        work("Finding") {
            do {
                listing = try await engine.hfModels(text, token: token)
                facts = [:]
                companionsOpen = false
            } catch let e as APIError where e.code == APIError.notARepo {
                listing = nil
                hits = try await engine.hfSearch(text, token: token)
            }
        }
    }

    private func check(_ repo: String, _ name: String) {
        work("Checking") { facts[name] = try await engine.hfInspect(repo, name: name, token: token) }
    }

    private func add(_ repo: String, _ name: String) {
        work("Adding") { try await engine.hfAdd(repo, name: name, token: token) }
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

/// What the server read off a model's header, and its verdict on this device, as it said it.
struct FactsView: View {
    let facts: HFFacts

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: Grid.s) {
                Badge(text: facts.mode, color: facts.mode == "streams" ? Palette.accent
                    : facts.readable && facts.mode != "not a model" ? Palette.muted : Palette.err)
                if FileRole.isCompanion(facts.role) { Badge(text: FileRole.label(facts.role), color: Palette.muted) }
                if !facts.arch.isEmpty { Text(facts.arch).font(.pixel(13)).foregroundStyle(Palette.muted) }
                if facts.isMoE {
                    Text("\(facts.nExpert) experts, \(facts.nExpertUsed) active").font(.pixel(13)).foregroundStyle(Palette.muted)
                }
            }
            if !facts.refusal.isEmpty { Text(facts.refusal).font(.caption).foregroundStyle(Palette.warn) }
            if !facts.disk.isEmpty { Text(facts.disk).font(.caption).foregroundStyle(facts.downloadable ? Palette.muted : Palette.err) }
            if !facts.memory.isEmpty { Text(facts.memory).font(.caption).foregroundStyle(Palette.muted) }
        }
        .fixedSize(horizontal: false, vertical: true)
    }
}
