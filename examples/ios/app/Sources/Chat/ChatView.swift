import BmoeKit
import SwiftUI

/// Chat (docs/screens.md, docs/agent.md): a conversation held by the server, with its items as
/// the run builds them (messages, the model's thinking, tool calls folded into what they did, the
/// approvals it waits on, helpers and notices), and the composer that says what the agent may do,
/// on which model, in which folder.
struct ChatView: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var agent: AgentModel
    @EnvironmentObject private var nav: Navigator
    var openModels: () -> Void

    @State private var showConversations = false
    @State private var preview: String?
    @State private var editing: AgentItem?

    var body: some View {
        VStack(spacing: 0) {
            transcript
            Composer()
        }
        .background(Palette.bg)
        .toolbar {
            ToolbarItem(placement: .navigationBarLeading) {
                Button { showConversations = true } label: { Image(systemName: "sidebar.left") }
                    .accessibilityLabel("Conversations")
            }
            ToolbarItem(placement: .navigationBarTrailing) {
                Button { agent.newChat() } label: { Image(systemName: "square.and.pencil") }
                    .accessibilityLabel("New chat")
            }
        }
        .sheet(isPresented: $showConversations) {
            ConversationsView().presentationDetents([.medium, .large])
        }
        .sheet(item: Binding(get: { preview.map(PreviewTarget.init) }, set: { preview = $0?.path })) { t in
            FilePreviewSheet(path: t.path)
        }
        .sheet(item: $editing) { item in
            EditMessageSheet(item: item)
        }
    }

    // MARK: the transcript

    private var transcript: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: Grid.m) {
                    if let note = agent.migrationNote {
                        Banner(text: note, color: Palette.accent) {
                            Button("Dismiss") { agent.migrationNote = nil }.buttonStyle(.appSecondary)
                        }
                    }
                    if agent.items.isEmpty && !agent.running && agent.decisions.isEmpty {
                        if agent.loadingConversation { ProgressView().frame(maxWidth: .infinity).padding(.top, Grid.xl) } else { empty }
                    }
                    ForEach(RunSummary.rows(agent.items)) { row in
                        ItemRow(row: row, openFile: { preview = $0 }, edit: { editing = $0 })
                    }
                    if agent.running && !(agent.items.last?.streaming ?? false) && agent.items.last?.isWaiting != true {
                        HStack(spacing: Grid.s) {
                            ProgressView().controlSize(.small)
                            Text(agent.items.last?.kind == .user ? "Reading the prompt…" : "Working…")
                                .font(.ui(14)).foregroundStyle(Palette.muted)
                        }
                    }
                    ForEach(agent.decisions) { DecisionView(card: $0) }
                    if agent.deciding {
                        HStack(spacing: Grid.s) {
                            ProgressView().controlSize(.small)
                            Text("Reading the options…").font(.ui(14)).foregroundStyle(Palette.muted)
                        }
                    }
                    if agent.running || agent.deciding { LiveLine() }
                    if let e = agent.error { errorBanner(e) }
                    Color.clear.frame(height: 1).id(Self.bottom)
                }
                .padding(Grid.m)
            }
            .scrollDismissesKeyboard(.interactively)
            .onChange(of: agent.items) { _ in proxy.scrollTo(Self.bottom, anchor: .bottom) }
            .onChange(of: agent.decisions.count) { _ in withAnimation { proxy.scrollTo(Self.bottom, anchor: .bottom) } }
        }
    }

    private static let bottom = "bottom"

    private var empty: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Logo(size: 48)
            Text(engine.ready || agent.route != AgentRoute.local ? "Ask anything." : "Load a model to start.")
                .font(.ui(22, weight: .semibold)).foregroundStyle(Palette.text)
            Text(agent.workspace == nil
                ? "Pick a workspace below and the agent can read and change the files in it, as the mode allows."
                : "The agent works in \(agent.workspace?.name ?? "the folder"): it reads freely and, in Ask first, asks before it changes a file.")
                .font(.ui(15)).foregroundStyle(Palette.muted)
            if !engine.ready && agent.route == AgentRoute.local {
                Button("Models", action: openModels).buttonStyle(.appPrimary).padding(.top, Grid.s)
            }
        }
        .padding(.top, Grid.xl)
    }

    /// Decided on by the error's code, never its wording (docs/server-api.md).
    private func errorBanner(_ e: TurnError) -> some View {
        let ctx = e.code == APIError.contextFull ? engine.contextAdvice : nil
        return Banner(text: ctx?.fullText ?? e.message, color: Palette.err) {
            HStack(spacing: Grid.s) {
                switch e.code {
                case APIError.contextFull:
                    Button("New chat") { agent.newChat() }.buttonStyle(.appPrimary)
                    if let ctx { Button("Raise \(ctx.label)") { nav.openSetting(ctx.key) }.buttonStyle(.appSecondary) }
                case APIError.noModel:
                    Button("Models", action: openModels).buttonStyle(.appPrimary)
                default:
                    EmptyView()
                }
                Button("Dismiss") { agent.error = nil }.buttonStyle(.appSecondary)
            }
        }
    }
}

private struct PreviewTarget: Identifiable {
    let path: String
    var id: String { path }
}

/// The composer: what the agent may do, on which model, in which folder, then the message.
struct Composer: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var agent: AgentModel
    @State private var input = ""
    @State private var choosing = false
    @State private var options = ""
    @State private var managing = false

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            controls
            if choosing {
                Text("Choose: the model picks one option, scored from one read of the prompt")
                    .font(.ui(13)).foregroundStyle(Palette.muted)
                AppField(placeholder: "Options, one per line", text: $options, axis: .vertical)
                    .lineLimit(2...6)
            }
            HStack(alignment: .bottom, spacing: Grid.s) {
                AppField(placeholder: choosing ? "The question" : "Message", text: $input, axis: .vertical)
                    .lineLimit(1...6)
                if agent.running {
                    Button { agent.stop() } label: { Image(systemName: "stop.fill").frame(width: 22, height: 22) }
                        .buttonStyle(.appDestructive)
                        .accessibilityLabel("Stop")
                } else {
                    Button(action: submit) { Image(systemName: "arrow.up").frame(width: 22, height: 22) }
                        .buttonStyle(.appPrimary)
                        .disabled(!canSubmit)
                        .accessibilityLabel(choosing ? "Choose" : "Send")
                }
            }
        }
        .padding(.horizontal, Grid.m)
        .padding(.vertical, 12)
        .background(Palette.surface)
        .overlay(alignment: .top) { Rectangle().fill(Palette.line).frame(height: Grid.line) }
        .sheet(isPresented: $managing) { NavigationStack { WorkspacesView(showsDone: true) } }
    }

    /// Three chips: the mode, the model route, the workspace. Each a native menu.
    private var controls: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: Grid.s) {
                Menu {
                    Picker("Mode", selection: Binding(get: { agent.mode }, set: { agent.setMode($0) })) {
                        ForEach(AgentMode.allCases) { m in
                            Label { Text(m.label); Text(m.help) } icon: { Image(systemName: icon(m)) }.tag(m)
                        }
                    }
                } label: { chip(agent.mode.label, icon: icon(agent.mode)) }

                Menu {
                    Picker("Model", selection: Binding(get: { agent.route }, set: { agent.setRoute($0) })) {
                        Text("This phone").tag(AgentRoute.local)
                        ForEach(agent.providers) { p in
                            ForEach(p.routes, id: \.route) { r in Text(r.label).tag(r.route) }
                        }
                    }
                } label: { chip(AgentRoute.label(agent.route, providers: agent.providers), icon: "cpu") }

                Menu {
                    Picker("Workspace", selection: Binding(get: { agent.workspaceID }, set: { agent.setWorkspace($0) })) {
                        Text("No workspace").tag("")
                        ForEach(agent.workspaces) { w in Text(w.name).tag(w.id) }
                    }
                    Divider()
                    Button { managing = true } label: { Label("Manage workspaces…", systemImage: "folder.badge.gearshape") }
                } label: { chip(agent.workspace?.name ?? "No workspace", icon: "folder") }

                Button { choosing.toggle() } label: {
                    chip(choosing ? "Choosing" : "Choose", icon: choosing ? "checklist.checked" : "checklist", on: choosing)
                }
                .accessibilityLabel(choosing ? "Back to chat" : "Choose from options")
            }
        }
    }

    private func icon(_ m: AgentMode) -> String {
        switch m {
        case .ask: return "hand.raised"
        case .readOnly: return "eye"
        case .full: return "bolt"
        }
    }

    private func chip(_ text: String, icon: String, on: Bool = false) -> some View {
        HStack(spacing: 4) {
            Image(systemName: icon).font(.ui(12))
            Text(text).font(.ui(13, weight: .medium)).lineLimit(1)
            Image(systemName: "chevron.down").font(.ui(11)).opacity(on ? 0 : 0.6)
        }
        .foregroundStyle(on ? Palette.accentInk : Palette.text)
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(Capsule().fill(on ? Palette.accentTint : Palette.surface2))
    }

    private var canSubmit: Bool {
        let hasText = !input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        guard engine.serverUp, hasText else { return false }
        if choosing { return engine.ready && !agent.deciding && Decide.parseOptions(options).count >= 2 }
        return !agent.sending
    }

    private func submit() {
        if choosing {
            agent.choose(input, options: Decide.parseOptions(options))
        } else {
            agent.send(input)
        }
        input = ""
    }
}

/// A decision's card: the question with its options, each with the probability the model put on
/// it, the picked one marked.
struct DecisionView: View {
    let card: DecisionCard

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Text(card.question).font(.ui(15)).foregroundStyle(Palette.text)
            ForEach(Array(card.result.scores.enumerated()), id: \.offset) { i, c in
                ChoiceRow(choice: c, picked: i == card.result.best)
            }
            if !card.result.metrics.isEmpty {
                Text(card.result.metrics).font(.figure(13)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
            }
            Text("A decision is not part of the conversation and is not kept.").font(.ui(12)).foregroundStyle(Palette.muted)
        }
        .card(padding: 12)
    }
}

/// One option of a decision, with the probability the model put on it; the picked one marked.
struct ChoiceRow: View {
    let choice: ChoiceScore
    let picked: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("\(choice.label)) \(choice.text)").font(.ui(15, weight: picked ? .semibold : .regular))
                    .foregroundStyle(picked ? Palette.accentInk : Palette.text)
                Spacer()
                Text(String(format: "%.1f%%", choice.probability * 100)).font(.figure(14)).foregroundStyle(Palette.muted)
            }
            Meter(fraction: choice.probability, color: picked ? Palette.accent : Palette.muted, height: 6)
        }
    }
}

/// The live line while the local model generates: the rate, the compute and flash shares, the
/// cache hit rate. The full card is in Metrics.
struct LiveLine: View {
    @EnvironmentObject private var engine: Engine

    var body: some View {
        let t = engine.live
        if t.step > 0 {
            let b = t.breakdown
            let share = { (ms: Double) in b.totalMs > 0 ? String(format: "%.0f%%", 100 * ms / b.totalMs) : "–" }
            HStack(spacing: Grid.m) {
                Text(String(format: "%.1f tok/s", t.rate))
                Text("compute \(share(b.computeMs))")
                Text("flash \(share(b.flashWaitMs))")
                if t.cacheHitPct >= 0 { Text(String(format: "cache %.0f%%", t.cacheHitPct)) }
            }
            .font(.figure(13))
            .foregroundStyle(Palette.muted)
        }
    }
}

/// The server's conversations, newest first: title, state, when it changed, the task that ran it.
/// Open, rename, delete, new.
struct ConversationsView: View {
    @EnvironmentObject private var agent: AgentModel
    @Environment(\.dismiss) private var dismiss
    @State private var renaming: ConversationSummary?
    @State private var newTitle = ""

    var body: some View {
        NavigationStack {
            List {
                ForEach(agent.conversations) { c in
                    Button {
                        agent.open(c.id)
                        dismiss()
                    } label: {
                        HStack(spacing: Grid.s) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(c.displayTitle).font(.ui(16)).foregroundStyle(Palette.text).lineLimit(1)
                                Text(subtitle(c)).font(.ui(12)).foregroundStyle(Palette.muted).lineLimit(1)
                            }
                            Spacer()
                            if c.state == "waiting" { Badge(text: "Needs you", color: Palette.warn) }
                            else if c.state == "running" { ProgressView().controlSize(.small) }
                            else if c.state == "error" { Badge(text: "Error", color: Palette.err) }
                        }
                    }
                    .listRowBackground(c.id == agent.current?.id ? Palette.accentTint : Palette.surface)
                    .swipeActions {
                        Button(role: .destructive) { agent.delete(c.id) } label: { Label("Delete", systemImage: "trash") }
                        Button {
                            newTitle = c.title
                            renaming = c
                        } label: { Label("Rename", systemImage: "pencil") }
                    }
                }
            }
            .scrollContentBackground(.hidden)
            .background(Palette.bg)
            .refreshable { await agent.refreshConversations() }
            .overlay {
                if agent.conversations.isEmpty {
                    Text("No conversations yet").font(.ui(15)).foregroundStyle(Palette.muted)
                }
            }
            .navigationTitle("Conversations")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("New chat") {
                        agent.newChat()
                        dismiss()
                    }
                }
            }
            .alert("Rename", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
                TextField("Title", text: $newTitle)
                Button("Save") { if let c = renaming { agent.rename(c.id, to: newTitle) } }
                Button("Cancel", role: .cancel) {}
            }
        }
    }

    private func subtitle(_ c: ConversationSummary) -> String {
        var parts: [String] = []
        if let u = c.updated { parts.append(u.formatted(.relative(presentation: .named))) }
        if let w = agent.workspaces.first(where: { $0.id == c.workspace }) { parts.append(w.name) }
        if !c.task.isEmpty { parts.append(agent.tasks.first { $0.id == c.task }?.title ?? "a task") }
        parts.append(c.mode.label)
        return parts.joined(separator: " · ")
    }
}

/// Edit and resend: the person's message, changed, replaces it and everything after it.
struct EditMessageSheet: View {
    @EnvironmentObject private var agent: AgentModel
    @Environment(\.dismiss) private var dismiss
    let item: AgentItem
    @State private var text = ""

    var body: some View {
        NavigationStack {
            TextEditor(text: $text)
                .font(.ui(17))
                .padding(Grid.s)
                .scrollContentBackground(.hidden)
                .background(Palette.surface)
                .navigationTitle("Edit and resend")
                .navigationBarTitleDisplayMode(.inline)
                .safeAreaInset(edge: .bottom) {
                    Text("What came after this message is dropped and the agent answers the new one.")
                        .font(.ui(13)).foregroundStyle(Palette.muted).padding(Grid.m)
                }
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Send") {
                            agent.edit(item, to: text)
                            dismiss()
                        }
                        .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    }
                }
        }
        .onAppear { text = item.text }
    }
}
