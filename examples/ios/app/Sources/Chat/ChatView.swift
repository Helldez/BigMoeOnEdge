import BmoeKit
import SwiftUI

/// Chat (docs/screens.md, docs/chat.md): a conversation held by the server, plain chat on a phone,
/// with its messages as the answer streams (the model's thinking folded above its answer, and the
/// server's notices), and the composer under them.
struct ChatView: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var chat: ChatModel
    @EnvironmentObject private var nav: Navigator
    var openModels: () -> Void

    @State private var showConversations = false
    @State private var editing: ConversationItem?

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
                Button { chat.newChat() } label: { Image(systemName: "square.and.pencil") }
                    .accessibilityLabel("New chat")
            }
        }
        .sheet(isPresented: $showConversations) {
            ConversationsView().presentationDetents([.medium, .large])
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
                    if let note = chat.migrationNote {
                        Banner(text: note, color: Palette.accent) {
                            Button("Dismiss") { chat.migrationNote = nil }.buttonStyle(.appSecondary)
                        }
                    }
                    if chat.items.isEmpty && !chat.running && chat.decisions.isEmpty {
                        if chat.loadingConversation { ProgressView().frame(maxWidth: .infinity).padding(.top, Grid.xl) } else { empty }
                    }
                    ForEach(chat.items) { item in
                        ItemView(item: item, answerAgain: item.id == chat.answerAgainOn, edit: { editing = $0 })
                    }
                    if chat.running && !(chat.items.last?.streaming ?? false) {
                        HStack(spacing: Grid.s) {
                            ProgressView().controlSize(.small)
                            Text(chat.items.last?.kind == .user ? "Reading the prompt…" : "Working…")
                                .font(.ui(14)).foregroundStyle(Palette.muted)
                        }
                    }
                    ForEach(chat.decisions) { DecisionView(card: $0) }
                    if chat.deciding {
                        HStack(spacing: Grid.s) {
                            ProgressView().controlSize(.small)
                            Text("Reading the options…").font(.ui(14)).foregroundStyle(Palette.muted)
                        }
                    }
                    if chat.running || chat.deciding { LiveLine() }
                    if let e = chat.error { errorBanner(e) }
                    Color.clear.frame(height: 1).id(Self.bottom)
                }
                .padding(Grid.m)
            }
            .scrollDismissesKeyboard(.interactively)
            .onChange(of: chat.items) { _ in proxy.scrollTo(Self.bottom, anchor: .bottom) }
            .onChange(of: chat.decisions.count) { _ in withAnimation { proxy.scrollTo(Self.bottom, anchor: .bottom) } }
        }
    }

    private static let bottom = "bottom"

    private var empty: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Logo(size: 48)
            Text(engine.ready ? "Ask anything." : "Load a model to start.")
                .font(.ui(22, weight: .semibold)).foregroundStyle(Palette.text)
            Text(engine.ready
                ? "The model runs on this phone, and nothing you write leaves it."
                : "The model runs on this phone, and nothing you write leaves it. Load one from Models, then write below.")
                .font(.ui(15)).foregroundStyle(Palette.muted)
            if !engine.ready {
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
                    Button("New chat") { chat.newChat() }.buttonStyle(.appPrimary)
                    if let ctx { Button("Raise \(ctx.label)") { nav.openSetting(ctx.key) }.buttonStyle(.appSecondary) }
                case APIError.noModel:
                    Button("Models", action: openModels).buttonStyle(.appPrimary)
                default:
                    EmptyView()
                }
                Button("Dismiss") { chat.error = nil }.buttonStyle(.appSecondary)
            }
        }
    }
}

/// The composer: the message, or a question and its options to choose from.
struct Composer: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var chat: ChatModel
    @State private var input = ""
    @State private var choosing = false
    @State private var options = ""

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
                if chat.running {
                    Button { chat.stop() } label: { Image(systemName: "stop.fill").frame(width: 22, height: 22) }
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
    }

    /// Choose from options, as a chip that turns the composer into a question and its options.
    private var controls: some View {
        HStack(spacing: Grid.s) {
            Button { choosing.toggle() } label: {
                chip(choosing ? "Choosing" : "Choose", icon: choosing ? "checklist.checked" : "checklist", on: choosing)
            }
            .accessibilityLabel(choosing ? "Back to chat" : "Choose from options")
        }
    }

    private func chip(_ text: String, icon: String, on: Bool = false) -> some View {
        HStack(spacing: 4) {
            Image(systemName: icon).font(.ui(12))
            Text(text).font(.ui(13, weight: .medium)).lineLimit(1)
        }
        .foregroundStyle(on ? Palette.accentInk : Palette.text)
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(Capsule().fill(on ? Palette.accentTint : Palette.surface2))
    }

    private var canSubmit: Bool {
        let hasText = !input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        guard engine.serverUp, hasText else { return false }
        if choosing { return engine.ready && !chat.deciding && Decide.parseOptions(options).count >= 2 }
        return !chat.sending
    }

    private func submit() {
        if choosing {
            chat.choose(input, options: Decide.parseOptions(options))
        } else {
            chat.send(input)
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

/// The server's conversations, newest first: title, state, when it changed. Open, rename, delete, new.
struct ConversationsView: View {
    @EnvironmentObject private var chat: ChatModel
    @Environment(\.dismiss) private var dismiss
    @State private var renaming: ConversationSummary?
    @State private var newTitle = ""

    var body: some View {
        NavigationStack {
            List {
                ForEach(chat.conversations) { c in
                    Button {
                        chat.open(c.id)
                        dismiss()
                    } label: {
                        HStack(spacing: Grid.s) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(c.displayTitle).font(.ui(16)).foregroundStyle(Palette.text).lineLimit(1)
                                if let u = c.updated {
                                    Text(u.formatted(.relative(presentation: .named))).font(.ui(12)).foregroundStyle(Palette.muted).lineLimit(1)
                                }
                            }
                            Spacer()
                            if c.state == "running" { ProgressView().controlSize(.small) }
                            else if c.state == "error" { Badge(text: "Error", color: Palette.err) }
                        }
                    }
                    .listRowBackground(c.id == chat.current?.id ? Palette.accentTint : Palette.surface)
                    .swipeActions {
                        Button(role: .destructive) { chat.delete(c.id) } label: { Label("Delete", systemImage: "trash") }
                        Button {
                            newTitle = c.title
                            renaming = c
                        } label: { Label("Rename", systemImage: "pencil") }
                    }
                }
            }
            .scrollContentBackground(.hidden)
            .background(Palette.bg)
            .refreshable { await chat.refreshConversations() }
            .overlay {
                if chat.conversations.isEmpty {
                    Text("No conversations yet").font(.ui(15)).foregroundStyle(Palette.muted)
                }
            }
            .navigationTitle("Conversations")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("New chat") {
                        chat.newChat()
                        dismiss()
                    }
                }
            }
            .alert("Rename", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
                TextField("Title", text: $newTitle)
                Button("Save") { if let c = renaming { chat.rename(c.id, to: newTitle) } }
                Button("Cancel", role: .cancel) {}
            }
        }
    }
}

/// Edit and send again: the person's message, changed, replaces it and everything after it.
struct EditMessageSheet: View {
    @EnvironmentObject private var chat: ChatModel
    @Environment(\.dismiss) private var dismiss
    let item: ConversationItem
    @State private var text = ""

    var body: some View {
        NavigationStack {
            TextEditor(text: $text)
                .font(.ui(17))
                .padding(Grid.s)
                .scrollContentBackground(.hidden)
                .background(Palette.surface)
                .navigationTitle("Edit and send again")
                .navigationBarTitleDisplayMode(.inline)
                .safeAreaInset(edge: .bottom) {
                    Text("What came after this message is dropped and the model answers the new one.")
                        .font(.ui(13)).foregroundStyle(Palette.muted).padding(Grid.m)
                }
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Send") {
                            chat.edit(item, to: text)
                            dismiss()
                        }
                        .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    }
                }
        }
        .onAppear { text = item.text }
    }
}
