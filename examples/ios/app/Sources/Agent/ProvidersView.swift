import BmoeKit
import SwiftUI

/// Providers (docs/agent.md, "Providers"): OpenAI-compatible chat services a conversation can run
/// on instead of the phone's own model. The key is written here and kept by the server; it is never
/// read back, so the screen only says whether one is saved.
struct ProvidersView: View {
    @EnvironmentObject private var agent: AgentModel
    @State private var editing: Provider?
    @State private var failure: String?

    var body: some View {
        List {
            Section {
                ForEach(agent.providers) { p in
                    VStack(alignment: .leading, spacing: 4) {
                        HStack {
                            Text(p.name).font(.ui(16, weight: .medium)).foregroundStyle(Palette.text)
                            Spacer()
                            Badge(text: p.hasKey ? "Key saved" : "No key", color: p.hasKey ? Palette.ok : Palette.muted)
                        }
                        Text(p.baseURL).font(.mono(12)).foregroundStyle(Palette.muted).lineLimit(1).truncationMode(.middle)
                        if !p.models.isEmpty {
                            Text(p.models.joined(separator: ", ")).font(.ui(13)).foregroundStyle(Palette.muted).lineLimit(2)
                        }
                    }
                    .contentShape(Rectangle())
                    .onTapGesture { editing = p }
                    .swipeActions {
                        Button(role: .destructive) {
                            Task {
                                do { try await agent.deleteProvider(p) } catch { failure = error.localizedDescription }
                            }
                        } label: { Label("Delete", systemImage: "trash") }
                    }
                }
                Button { editing = Provider() } label: { Label("Add a provider", systemImage: "plus") }
            } footer: {
                Text("A conversation on a provider's model sends its messages to that service; nothing else leaves this phone. "
                    + "Pick the model in the chat's composer.").font(.ui(13))
            }
            if let failure { Section { Text(failure).font(.ui(14)).foregroundStyle(Palette.err) } }
        }
        .scrollContentBackground(.hidden)
        .background(Palette.bg)
        .navigationTitle("Providers")
        .navigationBarTitleDisplayMode(.inline)
        .task { await agent.refreshProviders() }
        .sheet(item: $editing) { ProviderEditor(provider: $0) }
    }
}

struct ProviderEditor: View {
    @EnvironmentObject private var agent: AgentModel
    @Environment(\.dismiss) private var dismiss
    let provider: Provider
    @State private var name = ""
    @State private var baseURL = ""
    @State private var models = ""
    @State private var key = ""
    @State private var failure: String?
    @State private var saving = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Name", text: $name)
                    TextField("https://…/v1", text: $baseURL)
                        .font(.mono(15)).keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
                } header: { Text("Service") }
                Section {
                    TextField("model-a, model-b", text: $models, axis: .vertical)
                        .font(.mono(15)).textInputAutocapitalization(.never).autocorrectionDisabled()
                } header: { Text("Models") } footer: { Text("Separated by commas, as the service names them.") }
                Section {
                    SecureField(provider.hasKey ? "Saved: type to replace it" : "Key", text: $key)
                        .font(.mono(15)).textInputAutocapitalization(.never).autocorrectionDisabled()
                } header: { Text("Key") } footer: {
                    Text("Kept by the engine, readable by this app only, and never shown again. Leave it empty to keep the saved one.")
                }
                if let failure { Section { Text(failure).foregroundStyle(Palette.err) } }
            }
            .font(.ui(16))
            .navigationTitle(provider.id.isEmpty ? "New provider" : provider.name)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save", action: save)
                        .disabled(saving || name.trimmingCharacters(in: .whitespaces).isEmpty || !baseURL.hasPrefix("http"))
                }
            }
        }
        .onAppear {
            name = provider.name
            baseURL = provider.baseURL
            models = provider.models.joined(separator: ", ")
        }
    }

    private func save() {
        var p = provider
        p.name = name.trimmingCharacters(in: .whitespaces)
        p.baseURL = baseURL.trimmingCharacters(in: .whitespaces)
        p.models = models.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        saving = true
        Task {
            defer { saving = false }
            do {
                try await agent.saveProvider(p, key: key)
                key = ""
                dismiss()
            } catch {
                failure = error.localizedDescription
            }
        }
    }
}
