import BmoeKit
import SwiftUI

/// Skills (docs/agent.md, "Skills"): the instructions the agent reads when a job calls for them.
/// Every one can be switched off; the person's own can be written, changed and deleted here.
struct SkillsView: View {
    @EnvironmentObject private var agent: AgentModel
    @State private var failure: String?
    @State private var editing: Skill?

    var body: some View {
        List {
            Section {
                ForEach(agent.skills) { s in
                    HStack(alignment: .top, spacing: Grid.s) {
                        VStack(alignment: .leading, spacing: 4) {
                            HStack(spacing: 6) {
                                Text(s.title.isEmpty ? s.name : s.title).font(.ui(16, weight: .medium)).foregroundStyle(Palette.text)
                                Badge(text: source(s.source), color: s.editable ? Palette.accentInk : Palette.muted)
                            }
                            if !s.description.isEmpty {
                                Text(s.description).font(.ui(13)).foregroundStyle(Palette.muted).lineLimit(3)
                            }
                        }
                        Spacer()
                        Toggle("", isOn: Binding(get: { s.enabled }, set: { on in Task { await run { try await agent.setSkill(s, enabled: on) } } }))
                            .labelsHidden()
                            .accessibilityLabel("Use \(s.name)")
                    }
                    .contentShape(Rectangle())
                    .onTapGesture { if s.editable { editing = s } }
                    .swipeActions {
                        if s.editable {
                            Button(role: .destructive) { Task { await run { try await agent.deleteSkill(s) } } } label: {
                                Label("Delete", systemImage: "trash")
                            }
                        }
                    }
                }
                Button { editing = Skill(name: "") } label: { Label("Write a skill", systemImage: "square.and.pencil") }
            } footer: {
                Text("The agent sees each switched-on skill's name and description, and reads its instructions when a job "
                    + "calls for them. Skills that come with the app or with a workspace can be switched off, not changed.")
                    .font(.ui(13))
            }
            if let failure { Section { Text(failure).font(.ui(14)).foregroundStyle(Palette.err) } }
        }
        .tint(Palette.accent)
        .scrollContentBackground(.hidden)
        .background(Palette.bg)
        .navigationTitle("Skills")
        .navigationBarTitleDisplayMode(.inline)
        .task { await agent.refreshSkills() }
        .refreshable { await agent.refreshSkills() }
        .sheet(item: $editing) { SkillEditor(skill: $0) }
    }

    private func source(_ s: String) -> String {
        switch s {
        case "bundled": return "Built in"
        case "user": return "Yours"
        case "workspace": return "Workspace"
        default: return s
        }
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

/// Writing a skill: a kebab-case name, one line on when to use it, and the instructions.
struct SkillEditor: View {
    @EnvironmentObject private var agent: AgentModel
    @Environment(\.dismiss) private var dismiss
    let skill: Skill
    @State private var name = ""
    @State private var description = ""
    @State private var instructions = ""
    @State private var failure: String?
    @State private var saving = false

    private var isNew: Bool { skill.name.isEmpty }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("tidy-notes", text: $name)
                        .font(.mono(15))
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                } header: { Text("Name") } footer: {
                    Text(name.isEmpty || Skill.validName(name) ? "Lower-case words joined by hyphens."
                        : "Use lower-case letters, digits and single hyphens only.")
                        .foregroundStyle(name.isEmpty || Skill.validName(name) ? Palette.muted : Palette.err)
                }
                Section {
                    TextField("When the agent should use it", text: $description, axis: .vertical).lineLimit(2...4)
                } header: { Text("Description") }
                Section {
                    TextEditor(text: $instructions).font(.mono(14)).frame(minHeight: 220)
                } header: { Text("Instructions") }
                if let failure { Section { Text(failure).foregroundStyle(Palette.err) } }
            }
            .font(.ui(16))
            .navigationTitle(isNew ? "New skill" : skill.name)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save", action: save)
                        .disabled(saving || !Skill.validName(name) || description.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
        .onAppear {
            name = skill.name
            description = skill.description
            instructions = skill.body
        }
    }

    private func save() {
        saving = true
        Task {
            defer { saving = false }
            do {
                try await agent.saveSkill(name: name, description: description, body: instructions, existing: isNew ? nil : skill.name)
                dismiss()
            } catch {
                failure = error.localizedDescription
            }
        }
    }
}
