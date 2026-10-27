import BmoeKit
import SwiftUI
import UniformTypeIdentifiers

/// Workspaces on a phone: the server needs a real folder, and the folder there is is the app's own.
/// Each workspace is a folder in the app's Workspaces folder, which the Files app shows under this
/// app, so what the agent writes can be opened by any other app, and files put there by hand are
/// the agent's to read. Here a folder is made and named, its files listed, imported, shared out.
struct WorkspacesView: View {
    @EnvironmentObject private var agent: AgentModel
    @EnvironmentObject private var engine: Engine
    @Environment(\.dismiss) private var dismiss
    @State private var naming = false
    @State private var name = ""
    @State private var failure: String?
    @State private var removing: Workspace?
    /// Presented from the composer as a sheet it needs a Done; pushed from Settings it has Back.
    var showsDone = false

    var body: some View {
        List {
            Section {
                ForEach(agent.workspaces) { w in
                    NavigationLink { WorkspaceFilesView(workspace: w) } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(w.name).font(.ui(16)).foregroundStyle(Palette.text)
                            Text(location(w)).font(.mono(11)).foregroundStyle(Palette.muted).lineLimit(1).truncationMode(.middle)
                        }
                    }
                    .swipeActions {
                        Button(role: .destructive) { removing = w } label: { Label("Remove", systemImage: "folder.badge.minus") }
                    }
                }
                Button { naming = true } label: { Label("New folder", systemImage: "folder.badge.plus") }
                    .disabled(!engine.serverUp)
            } footer: {
                Text("A workspace is a folder the agent may work in. Here they live in this app's Workspaces folder, which "
                    + "the Files app shows: put files in there for the agent to read, and open what it writes from any app. "
                    + "Removing a workspace keeps its files.")
                    .font(.ui(13))
            }
            if let failure {
                Section { Text(failure).font(.ui(14)).foregroundStyle(Palette.err) }
            }
        }
        .scrollContentBackground(.hidden)
        .background(Palette.bg)
        .navigationTitle("Workspaces")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            if showsDone { ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } } }
        }
        .refreshable { await agent.refreshWorkspaces() }
        .task { await agent.refreshWorkspaces() }
        .alert("New folder", isPresented: $naming) {
            TextField("Name", text: $name)
            Button("Create") { create() }
            Button("Cancel", role: .cancel) { name = "" }
        } message: {
            Text("A folder in this app's Workspaces folder, added as a workspace.")
        }
        .confirmationDialog("Remove \(removing?.name ?? "")?", isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } }),
                            titleVisibility: .visible)
        {
            Button("Remove", role: .destructive) {
                if let w = removing { Task { await run { try await agent.removeWorkspace(w) } } }
            }
        } message: {
            Text("The agent stops seeing the folder; its files stay where they are.")
        }
    }


    private func location(_ w: Workspace) -> String {
        let root = engine.paths.documents.path
        guard w.path.hasPrefix(root) else { return w.path }
        let rest = String(w.path.dropFirst(root.count)).replacingOccurrences(of: "/", with: " › ")
        return "On My iPhone › BigMoeOnEdge" + rest
    }

    private func create() {
        let n = name
        name = ""
        Task { await run { _ = try await agent.createWorkspace(named: n) } }
    }

    private func run(_ body: () async throws -> Void) async {
        do {
            try await body()
            failure = nil
        } catch {
            failure = error.localizedDescription
        }
    }
}

/// One workspace's files: list them, open one, import from the Files app, share one out.
struct WorkspaceFilesView: View {
    let workspace: Workspace
    @State private var files: [URL] = []
    @State private var importing = false
    @State private var failure: String?
    @State private var shown: URL?

    private var folder: URL { URL(fileURLWithPath: workspace.path, isDirectory: true) }

    var body: some View {
        List {
            Section {
                if files.isEmpty {
                    Text("No files yet.").font(.ui(15)).foregroundStyle(Palette.muted)
                }
                ForEach(files, id: \.self) { f in
                    HStack {
                        Image(systemName: isDirectory(f) ? "folder" : "doc").foregroundStyle(Palette.muted)
                        Text(f.lastPathComponent).font(.ui(15)).foregroundStyle(Palette.text).lineLimit(1)
                        Spacer()
                        if !isDirectory(f) {
                            ShareLink(item: f) { Image(systemName: "square.and.arrow.up") }
                                .buttonStyle(.borderless)
                                .accessibilityLabel("Share \(f.lastPathComponent)")
                        }
                    }
                    .contentShape(Rectangle())
                    .onTapGesture { if !isDirectory(f) { shown = f } }
                    .swipeActions {
                        Button(role: .destructive) {
                            try? FileManager.default.removeItem(at: f)
                            reload()
                        } label: { Label("Delete", systemImage: "trash") }
                    }
                }
            } footer: {
                Text("Also in the Files app: On My iPhone › BigMoeOnEdge › Workspaces › \(folder.lastPathComponent).").font(.ui(13))
            }
            if let failure { Section { Text(failure).font(.ui(14)).foregroundStyle(Palette.err) } }
        }
        .scrollContentBackground(.hidden)
        .background(Palette.bg)
        .navigationTitle(workspace.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button { importing = true } label: { Label("Import", systemImage: "square.and.arrow.down") }
            }
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.item], allowsMultipleSelection: true) { result in
            switch result {
            case let .success(urls): copyIn(urls)
            case let .failure(e): failure = e.localizedDescription
            }
        }
        .sheet(item: Binding(get: { shown.map(LocalFile.init) }, set: { shown = $0?.url })) { f in
            LocalFilePreview(url: f.url)
        }
        .onAppear(perform: reload)
        .refreshable { reload() }
    }

    private func isDirectory(_ u: URL) -> Bool { (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false }

    private func reload() {
        let all = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.isDirectoryKey],
                                                                options: [.skipsHiddenFiles])) ?? []
        files = all.sorted { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
    }

    /// Copies what the person picked into the folder, keeping a same-named file by numbering the copy.
    private func copyIn(_ urls: [URL]) {
        let fm = FileManager.default
        var failed: [String] = []
        for u in urls {
            let scoped = u.startAccessingSecurityScopedResource()
            defer { if scoped { u.stopAccessingSecurityScopedResource() } }
            var dest = folder.appendingPathComponent(u.lastPathComponent)
            var n = 2
            while fm.fileExists(atPath: dest.path) {
                let base = u.deletingPathExtension().lastPathComponent
                let ext = u.pathExtension
                dest = folder.appendingPathComponent(ext.isEmpty ? "\(base) \(n)" : "\(base) \(n).\(ext)")
                n += 1
            }
            do { try fm.copyItem(at: u, to: dest) } catch { failed.append(u.lastPathComponent) }
        }
        failure = failed.isEmpty ? nil : "Not imported: " + failed.joined(separator: ", ")
        reload()
    }
}

private struct LocalFile: Identifiable {
    let url: URL
    var id: URL { url }
}

/// A file of a workspace read from the folder itself: the same preview as one the agent produced.
struct LocalFilePreview: View {
    let url: URL
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Group {
                if let data = try? Data(contentsOf: url) {
                    let f = PreviewFile(data: data, contentType: UTType(filenameExtension: url.pathExtension)?.preferredMIMEType ?? "",
                                        path: url.lastPathComponent)
                    switch f.kind {
                    case .markdown: ScrollView { MarkdownText(source: f.text).foregroundStyle(Palette.text).padding(Grid.m) }
                    case .html: SandboxedWebView(html: f.text)
                    case .image:
                        if let i = UIImage(data: data) { ScrollView { Image(uiImage: i).resizable().scaledToFit().padding(Grid.m) } }
                    case .text:
                        ScrollView([.vertical, .horizontal]) {
                            Text(f.text).font(.mono(13)).foregroundStyle(Palette.text).textSelection(.enabled).padding(Grid.m)
                        }
                    case .binary:
                        Text("No preview for this kind of file; share it to open it in another app.").font(.ui(15))
                            .foregroundStyle(Palette.muted).padding(Grid.m)
                    }
                } else {
                    Text("The file could not be read.").font(.ui(15)).foregroundStyle(Palette.err).padding(Grid.m)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .background(Palette.bg)
            .navigationTitle(url.lastPathComponent)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } }
                ToolbarItem(placement: .primaryAction) { ShareLink(item: url) }
            }
        }
    }
}
