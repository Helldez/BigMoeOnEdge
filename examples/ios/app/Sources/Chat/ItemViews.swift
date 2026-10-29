import BmoeKit
import SwiftUI

/// One item of a conversation, by its kind. `answerAgain`: this is the last answer, which offers
/// Answer again.
struct ItemView: View {
    let item: ConversationItem
    let answerAgain: Bool
    let edit: (ConversationItem) -> Void

    var body: some View {
        switch item.kind {
        case .user: UserMessageView(item: item, edit: edit)
        case .assistant: AssistantMessageView(item: item, answerAgain: answerAgain)
        case .notice: NoticeRow(item: item)
        case .other: EmptyView()
        }
    }
}

/// The person's words, verbatim, on the accent's tint at the trailing edge. Held, it offers Copy
/// and Edit and send again.
struct UserMessageView: View {
    @EnvironmentObject private var chat: ChatModel
    let item: ConversationItem
    let edit: (ConversationItem) -> Void

    var body: some View {
        HStack {
            Spacer(minLength: Grid.xl)
            Text(item.text)
                .font(.ui(16))
                .foregroundStyle(Palette.text)
                .padding(12)
                .background(RoundedRectangle(cornerRadius: Grid.radius * 1.5, style: .continuous).fill(Palette.accentTint))
                .contextMenu {
                    Button { UIPasteboard.general.string = item.text } label: { Label("Copy", systemImage: "doc.on.doc") }
                    Button { edit(item) } label: { Label("Edit and send again", systemImage: "pencil") }
                        .disabled(chat.running)
                }
        }
    }
}

/// The model's answer as Markdown, its thinking folded above; under it once it is whole, Copy,
/// and Answer again on the last one.
struct AssistantMessageView: View {
    @EnvironmentObject private var chat: ChatModel
    let item: ConversationItem
    let answerAgain: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            if !item.reasoning.isEmpty { ThinkingView(text: item.reasoning, streaming: item.streaming && item.text.isEmpty) }
            if !item.text.isEmpty { MarkdownText(source: item.text).foregroundStyle(Palette.text) }
            if !item.streaming {
                HStack(spacing: Grid.m) {
                    if !item.text.isEmpty {
                        Button { UIPasteboard.general.string = item.text } label: { Label("Copy", systemImage: "doc.on.doc") }
                    }
                    if answerAgain {
                        Button { chat.answerAgain() } label: { Label("Answer again", systemImage: "arrow.clockwise") }
                            .disabled(chat.running)
                    }
                }
                .font(.ui(13)).foregroundStyle(Palette.muted).buttonStyle(.borderless)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// A reasoning model's thinking span: open while it streams, folded once the answer starts.
struct ThinkingView: View {
    let text: String
    let streaming: Bool
    @State private var open: Bool?

    var body: some View {
        DisclosureGroup(isExpanded: Binding(get: { open ?? streaming }, set: { open = $0 })) {
            Text(text).font(.ui(14)).foregroundStyle(Palette.muted).textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.top, 4)
        } label: {
            Label(streaming ? "Thinking…" : "Thinking", systemImage: "brain").font(.ui(13, weight: .medium)).foregroundStyle(Palette.muted)
        }
        .tint(Palette.muted)
    }
}

/// Why something did not happen, in plain words, in its level's colour.
struct NoticeRow: View {
    let item: ConversationItem

    var body: some View {
        let color = item.level == "error" ? Palette.err : item.level == "warn" ? Palette.warn : Palette.muted
        HStack(alignment: .firstTextBaseline, spacing: Grid.s) {
            Image(systemName: item.level == "info" || item.level.isEmpty ? "info.circle" : "exclamationmark.triangle")
            Text(item.text).fixedSize(horizontal: false, vertical: true)
        }
        .font(.ui(13))
        .foregroundStyle(color)
        .padding(.horizontal, 12).padding(.vertical, Grid.s)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: Grid.radius, style: .continuous).fill(Palette.tint(color)))
    }
}
