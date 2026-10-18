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
        VStack(alignment: .leading, spacing: Grid.m) {
            SectionTitle("On this device", trailing: engine.models.local.isEmpty ? nil : "\(engine.models.local.count)")
            if engine.models.local.isEmpty {
                Text("No model yet. Download one below, add one from Hugging Face, or copy a .gguf into this app's "
                    + "folder with the Files app or the Finder.")
                    .font(.subheadline).foregroundStyle(Palette.muted)
            }
            ForEach(engine.models.local) { m in localRow(m) }
        }
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
        return VStack(alignment: .leading, spacing: Grid.s) {
            HStack(alignment: .firstTextBaseline) {
                Text(e.title).font(.headline).foregroundStyle(Palette.text)
                Spacer()
                Text([e.quant, ByteText.gb(e.bytes)].filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.pixel(14)).foregroundStyle(Palette.muted)
            }
            if !e.blurb.isEmpty { Text(e.blurb).font(.subheadline).foregroundStyle(Palette.muted) }
            if let p = progress {
                Meter(fraction: p.fraction)
                Text("\(ByteText.gb(p.received)) of \(ByteText.gb(p.total))").font(.pixel(13)).foregroundStyle(Palette.muted)
                if case let .failed(why) = p.state {
                    Banner(text: why, color: Palette.err) {
                        HStack(spacing: Grid.s) {
                            Button("Retry") { engine.download(e) }.buttonStyle(.pixel)
                            Button("Dismiss") { engine.dismissDownload(e.id) }.buttonStyle(.pixel)
                        }
                    }
                }
            }
            HStack(spacing: Grid.s) {
                switch status {
                case .downloading:
                    Button("Cancel") { engine.cancelDownload(e.id) }.buttonStyle(.pixel)
                case .onDevice:
                    Badge(text: "on device", color: Palette.ok)
                case .available:
                    Button("Download") { engine.download(e) }.buttonStyle(.pixelPrimary)
                }
                if e.added {
                    Spacer()
                    Button("Remove") { run { try await engine.removeAdded(e.id) } }.buttonStyle(.pixel)
                }
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
                        Text("\(r.downloads)↓").font(.pixel(13)).foregroundStyle(Palette.muted)
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
            ForEach(l.models) { m in
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
        }
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
                Badge(text: facts.mode, color: facts.mode == "streams" ? Palette.accent : facts.readable ? Palette.muted : Palette.err)
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
