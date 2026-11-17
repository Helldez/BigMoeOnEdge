import BmoeKit
import PhotosUI
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
    @State private var showSystem = false

    var body: some View {
        VStack(spacing: 0) {
            transcript
            Composer()
        }
        .background(Palette.bg)
        .toolbar {
            ToolbarItem(placement: .navigationBarLeading) {
                Button { showConversations = true } label: { Image(systemName: "sidebar.left") }
                    .tint(Palette.onShell)
                    .accessibilityLabel("Conversations")
            }
            ToolbarItem(placement: .navigationBarTrailing) {
                HStack {
                    Button { showSystem = true } label: { Image(systemName: "info.circle") }
                        .tint(Palette.onShell)
                        .accessibilityLabel("System prompt")
                    Button { chat.newChat() } label: { Image(systemName: "square.and.pencil") }
                        .tint(Palette.onShell)
                        .accessibilityLabel("New chat")
                }
            }
        }
        .sheet(isPresented: $showConversations) {
            ConversationsView().presentationDetents([.medium, .large])
        }
        .sheet(item: $editing) { item in
            EditMessageSheet(item: item)
        }
        .sheet(isPresented: $showSystem) {
            SystemPromptSheet(conversation: chat.current?.id, current: chat.system)
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
                            Text(chat.items.last?.kind == .user ? "Reading your message…" : "Working…")
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

    /// The welcome: the logo on a sheet over the seal's yellow rule, the promise that nothing
    /// leaves, and the question. A phone has no folders to give a job, so the line asks only for a
    /// question; with no model loaded the way to Models is under it (the composer says to load one).
    private var empty: some View {
        VStack(spacing: 12) {
            SealedLogo(size: 64).padding(.bottom, 4)
            // The state while there is one to say ("No model loaded", "Loading the model…"); the
            // promise that nothing leaves once a model can answer.
            let said = StatusWords.say(.of(engine: engine, chat: chat), greet: true)
            Text(said.text)
                .font(.ui(15, weight: .medium)).foregroundStyle(said.mood == .alert ? Palette.warn : Palette.muted)
            Text("What shall we do?")
                .font(.ui(22, weight: .semibold)).foregroundStyle(Palette.text)
                .padding(.top, Grid.s)
                .accessibilityAddTraits(.isHeader)
            Text("Ask a question.").font(.ui(15)).foregroundStyle(Palette.muted)
            if engine.ready && !chat.running && !chat.sending {
                // The first success is one tap away: a question that shows what the model does,
                // sent as it is, editable afterwards like any message.
                VStack(spacing: Grid.s) {
                    ForEach(Setup.tries, id: \.self) { q in
                        Button { Task { await chat.send(q) } } label: {
                            Text(q).font(.ui(15)).foregroundStyle(Palette.text).multilineTextAlignment(.leading)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(.horizontal, 14).padding(.vertical, 12)
                                .box(edge: Palette.line, radius: Grid.control)
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.top, Grid.m)
                .accessibilityElement(children: .contain)
                .accessibilityLabel("Questions to try")
            }
            if !engine.ready {
                Button("Models", action: openModels).buttonStyle(.appPrimary).padding(.top, Grid.s)
            }
        }
        .multilineTextAlignment(.center)
        .frame(maxWidth: .infinity)
        .padding(.top, 72)
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

/// The composer: the message with the images attached to it, or a question and its options to
/// choose from.
struct Composer: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var chat: ChatModel
    @State private var input = ""
    @State private var choosing = false
    @State private var options = ""
    /// The images going with the message, until the server takes it: a refused message keeps them.
    @State private var images: [PendingImage] = []
    @State private var imageError: String?
    @State private var picking = false
    @State private var picked: [PhotosPickerItem] = []
    @State private var takingPhoto = false

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            if engine.loading {
                Text("The model is loading; you can write as soon as it is ready.")
                    .font(.ui(13)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
            }
            if !images.isEmpty { attachments }
            if let imageError {
                Text(imageError).font(.ui(13)).foregroundStyle(Palette.err).fixedSize(horizontal: false, vertical: true)
            }
            if choosing {
                Text("Choose: the model picks one option, scored from one read of the prompt")
                    .font(.ui(13)).foregroundStyle(Palette.muted)
                AppField(placeholder: "Options, one per line", text: $options, axis: .vertical)
                    .lineLimit(2...6)
            }
            TextField(choosing ? "The question" : engine.ready || engine.loading ? "Message" : "Load a model to start", text: $input, axis: .vertical)
                .lineLimit(1...6)
                .font(.ui(17))
                .foregroundStyle(Palette.text)
                .tint(Palette.accent)
                .padding(.horizontal, 4)
                .padding(.top, 2)
            HStack(alignment: .center, spacing: Grid.s) {
                controls
                Spacer(minLength: 0)
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
        // A sheet of its own: the field's strong edge, a soft shadow, a control's corner.
        .padding(12)
        .box(edge: Palette.lineStrong, radius: Grid.control)
        .padding(.horizontal, Grid.m)
        .padding(.top, Grid.s)
        .padding(.bottom, 12)
        .background(Palette.bg)
        // The system's photo picker, up to what the message still has room for, images only.
        .photosPicker(isPresented: $picking, selection: $picked, maxSelectionCount: max(1, ImageAttachments.maxImages - images.count),
                      matching: .images)
        .onChange(of: picked) { items in
            guard !items.isEmpty else { return }
            picked = []
            Task { await attach(items) }
        }
        .fullScreenCover(isPresented: $takingPhoto) {
            CameraPicker { data in
                imageError = PickedImage.attach(mime: ImageAttachments.fallbackMIME, data: data, to: &images)
            }
            .ignoresSafeArea()
        }
    }

    /// What goes with the message: Choose from options, which turns the composer into a question
    /// and its options, and an image, from the library or the camera. The image entries stay in
    /// the menu when the model cannot read one, disabled, with the reason.
    private var controls: some View {
        HStack(spacing: Grid.s) {
            Menu {
                Button { choosing.toggle() } label: {
                    Label(choosing ? "Back to chat" : "Choose from options", systemImage: choosing ? "bubble.left" : "checklist")
                }
                Button { picking = true } label: {
                    Text("Image")
                    if let imageReason { Text(imageReason) }
                    Image(systemName: "photo")
                }
                .disabled(imageReason != nil)
                if CameraPicker.available {
                    Button { takingPhoto = true } label: {
                        Text("Take a photo")
                        if let imageReason { Text(imageReason) }
                        Image(systemName: "camera")
                    }
                    .disabled(imageReason != nil)
                }
            } label: {
                chip(choosing ? "Choosing" : "Add", icon: choosing ? "checklist.checked" : "plus", on: choosing)
            }
            .accessibilityLabel(choosing ? "Choosing from options" : "Add to the message")
        }
    }

    /// Why an image cannot be attached now: a decision is its question and options only; otherwise
    /// what the loaded model can read.
    private var imageReason: String? {
        choosing ? "An image goes with a message, not with a choice" : chat.imageReason
    }

    /// The images attached so far, each with its remove button, above the words.
    private var attachments: some View {
        WrapRow {
            ForEach(images) { image in
                DataURLThumbnail(dataURL: image.dataURL, side: 64)
                    .overlay(alignment: .topTrailing) {
                        Button {
                            images.removeAll { $0.id == image.id }
                            imageError = nil
                        } label: {
                            Image(systemName: "xmark.circle.fill")
                                .font(.ui(18))
                                .foregroundStyle(Palette.text, Palette.surface)
                                .padding(2)
                        }
                        .accessibilityLabel("Remove image")
                    }
            }
        }
    }

    /// Reads what was picked and attaches each under the composer's rules; the first refusal is
    /// shown and the rest still go in, until the message is full (nothing more is read then).
    private func attach(_ items: [PhotosPickerItem]) async {
        var refused: String?
        for item in items {
            if images.count >= ImageAttachments.maxImages {
                refused = refused ?? ImageAttachments.accept(mime: ImageAttachments.fallbackMIME, bytes: 0, count: images.count)
                break
            }
            // A library item that loads nothing: a photo still in iCloud, or one the system could
            // not hand over.
            guard let loaded = await PickedImage.load(item) else {
                refused = refused ?? "The image could not be read"
                continue
            }
            if let why = PickedImage.attach(mime: loaded.mime, data: loaded.data, to: &images) { refused = refused ?? why }
        }
        imageError = refused
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
        guard engine.serverUp else { return false }
        if choosing { return hasText && engine.ready && !chat.deciding && Decide.parseOptions(options).count >= 2 }
        return (hasText || !images.isEmpty) && !chat.sending
    }

    private func submit() {
        if choosing {
            chat.choose(input, options: Decide.parseOptions(options))
            input = ""
            return
        }
        // The words and the images stay in the composer until the server takes them: a refused
        // message (a model without its projector, an image too large to decode) is still there to
        // change and send again, with the server's reason in the error banner.
        let text = input, sent = images
        imageError = nil
        Task {
            if await chat.send(text, images: sent.map(\.dataURL)) {
                input = ""
                images.removeAll { image in sent.contains { $0.id == image.id } }
            }
        }
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
                                Text(c.displayTitle).font(.ui(16, weight: c.id == chat.current?.id ? .semibold : .regular))
                                    .foregroundStyle(Palette.onShell).lineLimit(1)
                                if let u = c.updated {
                                    Text(u.formatted(.relative(presentation: .named))).font(.ui(12)).foregroundStyle(Palette.onShellMuted).lineLimit(1)
                                }
                            }
                            Spacer()
                            if c.state == "running" { ProgressView().controlSize(.small).tint(Palette.onShell) }
                            else if c.state == "error" { Badge(text: "Error", color: Palette.err) }
                        }
                    }
                    .listRowBackground(c.id == chat.current?.id ? Palette.shellHi : Palette.shell)
                    .listRowSeparatorTint(Palette.shellHi)
                    .accessibilityAddTraits(c.id == chat.current?.id ? .isSelected : [])
                    .swipeActions {
                        Button(role: .destructive) { chat.delete(c.id) } label: { Label("Delete", systemImage: "trash") }
                        Button {
                            newTitle = c.title
                            renaming = c
                        } label: { Label("Rename", systemImage: "pencil") }
                    }
                }
            }
            // The drawer of the other UIs: the navigation's ink, the open conversation raised in it.
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .background(Palette.shell.ignoresSafeArea())
            .refreshable { await chat.refreshConversations() }
            .overlay {
                if chat.conversations.isEmpty {
                    Text("No conversations yet").font(.ui(15)).foregroundStyle(Palette.onShellMuted)
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
                    .tint(Palette.onShell)
                }
            }
            .shellBar()
            .alert("Rename", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
                TextField("Title", text: $newTitle)
                Button("Save") { if let c = renaming { chat.rename(c.id, to: newTitle) } }
                Button("Cancel", role: .cancel) {}
            }
        }
    }
}

/// The conversation's system prompt (#625), or the new chat's when `conversation` is nil: the
/// person's own, written here, and what the model reads before the conversation, exactly as the next
/// message sends it. Without a folder the person's text is all the model gets, as in llama.cpp's web UI.
struct SystemPromptSheet: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var chat: ChatModel
    @Environment(\.dismiss) private var dismiss
    let conversation: String?
    let current: String
    @State private var prompt: SystemPrompt?
    @State private var own = ""
    @State private var error = ""
    @State private var saving = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: Grid.s) {
                    if prompt?.external == true {
                        Text("This conversation runs another agent program, which writes its own system prompt.").font(.ui(15))
                    } else {
                        Text("Instructions the model reads before this conversation. Leave it empty for none.")
                            .font(.ui(13)).foregroundStyle(Palette.muted)
                        TextEditor(text: $own)
                            .font(.ui(15))
                            .frame(minHeight: 140)
                            .padding(Grid.s / 2)
                            .overlay(RoundedRectangle(cornerRadius: Grid.radius).stroke(Palette.muted.opacity(0.4)))
                            .disabled(saving)
                            .accessibilityLabel("System prompt")
                        if !error.isEmpty { Text(error).font(.ui(13)).foregroundStyle(Palette.warn) }
                        if let p = prompt, !p.added.isEmpty {
                            Text("What the model reads, with the folder's agent lines first:").font(.ui(13)).foregroundStyle(Palette.muted)
                            Text(p.added).font(.system(size: 13, design: .monospaced)).textSelection(.enabled)
                        }
                        if let n = prompt?.tokens {
                            Text("\(n) token\(n == 1 ? "" : "s") of the context").font(.ui(13)).foregroundStyle(Palette.muted)
                        }
                    }
                }
                .padding(Grid.m)
            }
            .background(Palette.surface)
            .navigationTitle("System prompt")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } }
                if prompt?.external != true {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Save") { Task { await save() } }.disabled(own == current || saving)
                    }
                }
            }
        }
        .task { await load() }
    }

    private func load() async {
        own = current
        guard let id = conversation else { return }
        do {
            let p = try await engine.api.systemPrompt(conversation: id)
            prompt = p
            own = p.own
        } catch {
            self.error = "Could not read the system prompt: \(error.localizedDescription)"
        }
    }

    private func save() async {
        saving = true
        error = ""
        defer { saving = false }
        guard await chat.setSystem(own) else {
            error = "Could not save the system prompt: \(chat.error?.message ?? "")"
            return
        }
        if conversation == nil { dismiss() } else { await load() }
    }
}

/// Edit and send again: the person's message, changed, replaces it and everything after it. The
/// images it carried go again as they were (the web UI's rule: an edit changes the words).
struct EditMessageSheet: View {
    @EnvironmentObject private var chat: ChatModel
    @Environment(\.dismiss) private var dismiss
    let item: ConversationItem
    @State private var text = ""

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: Grid.s) {
                if !item.images.isEmpty {
                    WrapRow {
                        ForEach(Array(item.images.enumerated()), id: \.offset) { _, url in DataURLThumbnail(dataURL: url, side: 64) }
                    }
                    .padding(.horizontal, Grid.m).padding(.top, Grid.s)
                }
                TextEditor(text: $text)
                    .font(.ui(17))
                    .padding(Grid.s)
                    .scrollContentBackground(.hidden)
            }
            .background(Palette.surface)
            .navigationTitle("Edit and send again")
            .navigationBarTitleDisplayMode(.inline)
            .safeAreaInset(edge: .bottom) {
                Text(item.images.isEmpty
                    ? "What came after this message is dropped and the model answers the new one."
                    : "Everything after this message is dropped, then the edited one is sent, with the same images.")
                    .font(.ui(13)).foregroundStyle(Palette.muted).padding(Grid.m)
            }
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() }.tint(Palette.onShell) }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Send") {
                        chat.edit(item, to: text, images: item.images)
                        dismiss()
                    }
                    .tint(Palette.onShell)
                    .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && item.images.isEmpty)
                }
            }
            .shellBar()
        }
        .onAppear { text = item.text }
    }
}
