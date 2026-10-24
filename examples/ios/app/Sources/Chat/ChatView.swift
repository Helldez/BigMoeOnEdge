import BmoeKit
import SwiftUI

/// Chat (docs/screens.md): a conversation with the loaded model, kept on the device, with the
/// model's reasoning visible and every answer's cost visible.
struct ChatView: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var chat: ChatModel
    @EnvironmentObject private var nav: Navigator
    var openModels: () -> Void

    @State private var input = ""
    @State private var choosing = false
    @State private var options = ""
    @State private var showConversations = false
    @FocusState private var focused: Bool

    var body: some View {
        VStack(spacing: 0) {
            transcript
            composer
        }
        .background(Palette.bg)
        .toolbar {
            ToolbarItem(placement: .navigationBarLeading) {
                Button { showConversations = true } label: { Image(systemName: "list.bullet") }
                    .accessibilityLabel("Conversations")
            }
            ToolbarItem(placement: .navigationBarTrailing) {
                Button { chat.newConversation() } label: { Image(systemName: "square.and.pencil") }
                    .disabled(chat.streaming)
                    .accessibilityLabel("New chat")
            }
        }
        .sheet(isPresented: $showConversations) {
            ConversationsView().presentationDetents([.medium, .large])
        }
    }

    // MARK: the transcript

    private var transcript: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: Grid.m) {
                    if chat.current.messages.isEmpty && !chat.streaming { empty }
                    ForEach(chat.current.messages) { m in
                        MessageView(message: m, isLast: m.id == chat.current.messages.last?.id)
                    }
                    if chat.streaming { streamingTurn }
                    if let e = chat.error { errorBanner(e) }
                    Color.clear.frame(height: 1).id(Self.bottom)
                }
                .padding(Grid.m)
                .padding(.trailing, Grid.shadow)
            }
            .scrollDismissesKeyboard(.interactively)
            .onChange(of: chat.answer) { _ in proxy.scrollTo(Self.bottom, anchor: .bottom) }
            .onChange(of: chat.reasoning) { _ in proxy.scrollTo(Self.bottom, anchor: .bottom) }
            .onChange(of: chat.current.messages.count) { _ in
                withAnimation { proxy.scrollTo(Self.bottom, anchor: .bottom) }
            }
        }
    }

    private static let bottom = "bottom"

    private var empty: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Logo(size: 48)
            Text(engine.ready ? "Ask anything." : "Load a model to start.").font(.pixel(20, bold: true))
                .foregroundStyle(Palette.text)
            Text("The conversation stays on this device. A follow-up costs only its own message: the engine keeps "
                + "what it has already read.")
                .font(.subheadline).foregroundStyle(Palette.muted)
            if !engine.ready { Button("Models", action: openModels).buttonStyle(.pixelPrimary).padding(.top, Grid.s) }
        }
        .padding(.top, Grid.xl)
    }

    private var streamingTurn: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            if !chat.reasoning.isEmpty { ThinkingView(text: chat.reasoning, streaming: true) }
            if chat.answer.isEmpty {
                Text(chat.reasoning.isEmpty ? "Reading the prompt…" : "Thinking…")
                    .font(.pixel(14)).foregroundStyle(Palette.muted)
            } else {
                MarkdownText(source: chat.answer).foregroundStyle(Palette.text)
            }
            LiveLine()
        }
        .modifier(AssistantBubble())
    }

    /// Decided on by the error's code, never its wording (docs/server-api.md).
    private func errorBanner(_ e: TurnError) -> some View {
        // A full context is fixed by a new chat or a larger context; the server says which setting that is.
        let ctx = e.code == APIError.contextFull ? engine.contextAdvice : nil
        return Banner(text: ctx?.fullText ?? e.message, color: Palette.err) {
            HStack(spacing: Grid.s) {
                switch e.code {
                case APIError.contextFull:
                    Button("New chat") { chat.newConversation() }.buttonStyle(.pixelPrimary)
                    if let ctx { Button("Raise \(ctx.label)") { nav.openSetting(ctx.key) }.buttonStyle(.pixel) }
                case APIError.noModel:
                    Button("Models", action: openModels).buttonStyle(.pixelPrimary)
                default:
                    EmptyView()
                }
                if chat.canRetry && e.code != APIError.contextFull {
                    Button("Send again") { chat.retry() }.buttonStyle(.pixel)
                }
                Button("Dismiss") { chat.error = nil }.buttonStyle(.pixel)
            }
        }
    }

    // MARK: the composer

    private var composer: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            if choosing {
                Text("CHOOSE: the model picks one option, scored from one read of the prompt")
                    .font(.pixel(13)).foregroundStyle(Palette.muted)
                PixelField(placeholder: "Options, one per line", text: $options, axis: .vertical)
                    .lineLimit(2...6)
            }
            HStack(alignment: .bottom, spacing: Grid.s) {
                Button { choosing.toggle() } label: {
                    Image(systemName: choosing ? "checklist.checked" : "checklist")
                        .frame(width: 22, height: 22)
                }
                .buttonStyle(.pixel)
                .accessibilityLabel(choosing ? "Back to chat" : "Choose from options")

                PixelField(placeholder: choosing ? "The question" : "Message", text: $input, axis: .vertical)
                    .lineLimit(1...6)
                    .focused($focused)

                if chat.streaming {
                    Button("Stop") { chat.stop() }.buttonStyle(.pixelDestructive)
                } else {
                    Button(choosing ? "Choose" : "Send", action: submit)
                        .buttonStyle(.pixelPrimary)
                        .disabled(!canSubmit)
                }
            }
        }
        .padding(Grid.m)
        .padding(.trailing, Grid.shadow)
        .background(Palette.surface)
        .overlay(alignment: .top) { Rectangle().fill(Palette.border).frame(height: Grid.edge) }
    }

    private var canSubmit: Bool {
        let hasText = !input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        return engine.ready && hasText && (!choosing || Decide.parseOptions(options).count >= 2)
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

/// One message: the person's text verbatim, the model's as Markdown with its thinking folded above
/// and its figures under it, or a decision's options with their probabilities.
struct MessageView: View {
    @EnvironmentObject private var chat: ChatModel
    let message: ChatMessage
    let isLast: Bool

    var body: some View {
        if message.role == .user {
            HStack {
                Spacer(minLength: Grid.xl)
                Text(message.content)
                    .textSelection(.enabled)
                    .foregroundStyle(Palette.text)
                    .padding(12)
                    .pixelBox(fill: Palette.surface2, edge: Palette.border, shadow: false)
            }
        } else if let choices = message.choices {
            VStack(alignment: .leading, spacing: Grid.s) {
                ForEach(Array(choices.enumerated()), id: \.offset) { i, c in
                    ChoiceRow(choice: c, picked: i == message.best)
                }
                if !message.metrics.isEmpty { metricsLine }
            }
            .modifier(AssistantBubble())
        } else {
            VStack(alignment: .leading, spacing: Grid.s) {
                if !message.reasoning.isEmpty { ThinkingView(text: message.reasoning, streaming: false) }
                MarkdownText(source: message.content).foregroundStyle(Palette.text)
                if !message.metrics.isEmpty { metricsLine }
                HStack(spacing: Grid.s) {
                    Button { UIPasteboard.general.string = message.content } label: { Label("Copy", systemImage: "doc.on.doc") }
                    if isLast && chat.canRegenerate {
                        Button { chat.regenerate() } label: { Label("Regenerate", systemImage: "arrow.clockwise") }
                    }
                }
                .font(.pixel(13))
                .foregroundStyle(Palette.muted)
                .buttonStyle(.borderless)
            }
            .modifier(AssistantBubble())
        }
    }

    private var metricsLine: some View {
        Text(message.metrics).font(.pixel(13)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
    }
}

/// The model's side of a conversation: the surface, its edge in the model's colour.
struct AssistantBubble: ViewModifier {
    func body(content: Content) -> some View {
        content
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .pixelBox(fill: Palette.surface, edge: Palette.accent2, shadow: false)
    }
}

/// A reasoning model's thinking span: open while it streams, folded once committed.
struct ThinkingView: View {
    let text: String
    let streaming: Bool
    @State private var open: Bool?

    var body: some View {
        DisclosureGroup(isExpanded: Binding(get: { open ?? streaming }, set: { open = $0 })) {
            Text(text).font(.callout).foregroundStyle(Palette.muted).textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.top, 4)
        } label: {
            Text("Thinking").font(.pixel(13, bold: true)).foregroundStyle(Palette.accent2)
        }
        .tint(Palette.accent2)
    }
}

/// One option of a decision, with the probability the model put on it; the picked one marked.
struct ChoiceRow: View {
    let choice: ChoiceScore
    let picked: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("\(choice.label)) \(choice.text)").foregroundStyle(picked ? Palette.accent : Palette.text)
                    .fontWeight(picked ? .bold : .regular)
                Spacer()
                Text(String(format: "%.1f%%", choice.probability * 100)).font(.pixel(14)).foregroundStyle(Palette.muted)
                    .monospacedDigit()
            }
            Meter(fraction: choice.probability, color: picked ? Palette.accent : Palette.accent2, height: 8)
        }
    }
}

/// The live line while an answer streams: the rate, the compute and flash shares, the cache hit
/// rate. The full card is in Metrics.
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
            .font(.pixel(13))
            .foregroundStyle(Palette.muted)
            .monospacedDigit()
        }
    }
}

/// The saved conversations, newest first: title, when it was answered, how many messages, which
/// model. Open, delete, new.
struct ConversationsView: View {
    @EnvironmentObject private var chat: ChatModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                ForEach(chat.conversations) { c in
                    Button {
                        chat.select(c)
                        dismiss()
                    } label: {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(c.title).foregroundStyle(Palette.text).lineLimit(1)
                            Text(subtitle(c)).font(.caption).foregroundStyle(Palette.muted).lineLimit(1)
                        }
                    }
                    .listRowBackground(c.id == chat.current.id ? Palette.surface2 : Palette.surface)
                }
                .onDelete { idx in idx.map { chat.conversations[$0].id }.forEach(chat.delete) }
            }
            .scrollContentBackground(.hidden)
            .background(Palette.bg)
            .overlay {
                if chat.conversations.isEmpty {
                    Text("No conversations yet").font(.pixel(15)).foregroundStyle(Palette.muted)
                }
            }
            .navigationTitle("Conversations")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("New chat") {
                        chat.newConversation()
                        dismiss()
                    }
                    .disabled(chat.streaming)
                }
            }
        }
    }

    private func subtitle(_ c: Conversation) -> String {
        let when = c.updated.formatted(.relative(presentation: .named))
        let parts = [when, "\(c.messages.count) messages", c.model].filter { !$0.isEmpty }
        return parts.joined(separator: " · ")
    }
}
