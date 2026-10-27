import BmoeKit
import SwiftUI

/// One row of the transcript: an item, or a run of tool calls folded under what they did.
struct ItemRow: View {
    let row: TranscriptRow
    let openFile: (String) -> Void
    let edit: (AgentItem) -> Void

    var body: some View {
        switch row {
        case let .item(item):
            ItemView(item: item, openFile: openFile, edit: edit)
        case let .group(_, summary, items):
            ToolGroupView(summary: summary, items: items, openFile: openFile)
        }
    }
}

struct ItemView: View {
    let item: AgentItem
    let openFile: (String) -> Void
    let edit: (AgentItem) -> Void

    var body: some View {
        switch item.kind {
        case .user: UserMessageView(item: item, edit: edit)
        case .assistant: AssistantMessageView(item: item)
        case .tool: item.isWaiting ? AnyView(ApprovalCard(item: item, openFile: openFile)) : AnyView(ToolCard(item: item, openFile: openFile))
        case .helper: HelperRow(item: item)
        case .notice: NoticeRow(item: item)
        case .other: EmptyView()
        }
    }
}

/// The person's words, verbatim, on the accent's tint at the trailing edge. Held, it offers Copy
/// and Edit and resend.
struct UserMessageView: View {
    @EnvironmentObject private var agent: AgentModel
    let item: AgentItem
    let edit: (AgentItem) -> Void

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
                    Button { edit(item) } label: { Label("Edit and resend", systemImage: "pencil") }
                        .disabled(agent.running)
                }
        }
    }
}

/// The model's answer as Markdown, its thinking folded above, Copy under it once it is whole.
struct AssistantMessageView: View {
    let item: AgentItem

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            if !item.reasoning.isEmpty { ThinkingView(text: item.reasoning, streaming: item.streaming && item.text.isEmpty) }
            if !item.text.isEmpty { MarkdownText(source: item.text).foregroundStyle(Palette.text) }
            if !item.streaming && !item.text.isEmpty {
                Button { UIPasteboard.general.string = item.text } label: { Label("Copy", systemImage: "doc.on.doc") }
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

/// The colour and words of a tool's or a helper's state.
enum ToolState {
    static func color(_ s: String) -> Color {
        switch s {
        case "done": return Palette.ok
        case "running": return Palette.accent
        case "waiting": return Palette.warn
        case "denied", "error": return Palette.err
        default: return Palette.muted
        }
    }

    static func label(_ s: String) -> String {
        switch s {
        case "done": return "Done"
        case "running": return "Running"
        case "waiting": return "Waiting for you"
        case "denied": return "Denied"
        case "error": return "Failed"
        default: return s.capitalized
        }
    }

    static func icon(_ tool: String) -> String {
        switch tool {
        case "read_file": return "doc.text"
        case "list_files": return "folder"
        case "search_files": return "magnifyingglass"
        case "write_file": return "doc.badge.plus"
        case "edit_file": return "pencil"
        case "fetch_url": return "globe"
        case "web_search": return "network"
        case "use_skill": return "book"
        case "delegate": return "person.2"
        default: return "wrench.and.screwdriver"
        }
    }
}

/// A tool call: what it did in plain words, its state, and folded under it the result, the diff
/// of an edit, and the file it produced to open.
struct ToolCard: View {
    let item: AgentItem
    let openFile: (String) -> Void
    @State private var open = false

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Button { withAnimation(.easeInOut(duration: 0.15)) { open.toggle() } } label: {
                HStack(spacing: Grid.s) {
                    Image(systemName: ToolState.icon(item.tool)).foregroundStyle(Palette.muted).frame(width: 18)
                    Text(item.title.isEmpty ? item.tool : item.title).font(.ui(14, weight: .medium)).foregroundStyle(Palette.text)
                        .lineLimit(open ? nil : 1).multilineTextAlignment(.leading)
                    Spacer(minLength: Grid.s)
                    if item.state == "running" { ProgressView().controlSize(.mini) } else {
                        Badge(text: ToolState.label(item.state), color: ToolState.color(item.state))
                    }
                    if hasDetail {
                        Image(systemName: "chevron.right").font(.caption.bold()).foregroundStyle(Palette.muted)
                            .rotationEffect(.degrees(open ? 90 : 0))
                    }
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(!hasDetail)
            if open {
                if !item.diff.isEmpty { DiffView(diff: item.diff) }
                if !item.result.isEmpty {
                    ScrollView {
                        Text(item.result).font(.mono(12)).foregroundStyle(Palette.text).textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading).padding(Grid.s)
                    }
                    .frame(maxHeight: 220)
                    .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(Palette.surface2))
                }
            }
            if !item.file.isEmpty {
                Button { openFile(item.file) } label: { Label("Open \(item.file)", systemImage: "doc.richtext") }
                    .font(.ui(13, weight: .medium)).foregroundStyle(Palette.accentInk).buttonStyle(.borderless).lineLimit(1)
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .box(lifted: false)
    }

    private var hasDetail: Bool { !item.diff.isEmpty || !item.result.isEmpty }
}

/// A run of tool calls, folded under what they did ("Read 3 files, edited 2 files").
struct ToolGroupView: View {
    let summary: String
    let items: [AgentItem]
    let openFile: (String) -> Void
    @State private var open = false

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Button { withAnimation(.easeInOut(duration: 0.15)) { open.toggle() } } label: {
                HStack(spacing: Grid.s) {
                    Image(systemName: "chevron.right").font(.caption.bold()).rotationEffect(.degrees(open ? 90 : 0))
                    Text(summary).font(.ui(14, weight: .medium))
                    Spacer()
                    if items.contains(where: { $0.state == "running" }) { ProgressView().controlSize(.mini) }
                    if items.contains(where: { $0.state == "error" || $0.state == "denied" }) {
                        Image(systemName: "exclamationmark.circle").foregroundStyle(Palette.err)
                    }
                }
                .foregroundStyle(Palette.muted)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if open {
                ForEach(items) { ToolCard(item: $0, openFile: openFile) }
            } else {
                // A file a folded call produced stays one tap away.
                ForEach(items.filter { !$0.file.isEmpty }) { t in
                    Button { openFile(t.file) } label: { Label("Open \(t.file)", systemImage: "doc.richtext") }
                        .font(.ui(13, weight: .medium)).foregroundStyle(Palette.accentInk).buttonStyle(.borderless).lineLimit(1)
                }
            }
        }
    }
}

/// The question the agent waits on: what it wants to do, the diff when it is an edit, and
/// Approve or Deny. Nothing has run when it appears.
struct ApprovalCard: View {
    @EnvironmentObject private var agent: AgentModel
    let item: AgentItem
    let openFile: (String) -> Void
    @State private var answered = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label("The agent asks first", systemImage: "hand.raised.fill").font(.ui(13, weight: .semibold)).foregroundStyle(Palette.warn)
            Text(item.title.isEmpty ? item.tool : item.title).font(.ui(16, weight: .semibold)).foregroundStyle(Palette.text)
            if !item.diff.isEmpty { DiffView(diff: item.diff) }
            else if let content = item.args["content"].string, !content.isEmpty {
                Text(content.prefix(1200)).font(.mono(12)).foregroundStyle(Palette.text).lineLimit(14)
                    .frame(maxWidth: .infinity, alignment: .leading).padding(Grid.s)
                    .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(Palette.surface2))
            }
            HStack(spacing: Grid.s) {
                Button("Approve") {
                    answered = true
                    agent.answer(item, approve: true)
                }
                .buttonStyle(.appPrimary)
                Button("Deny") {
                    answered = true
                    agent.answer(item, approve: false)
                }
                .buttonStyle(.appSecondary)
            }
            .disabled(answered)
        }
        .padding(Grid.m)
        .frame(maxWidth: .infinity, alignment: .leading)
        .box(fill: Palette.surface, edge: Palette.warn)
    }
}

/// A unified diff, a line at a time: added lines on the ok tint, removed on the error tint.
struct DiffView: View {
    let diff: String

    var body: some View {
        ScrollView([.horizontal, .vertical]) {
            VStack(alignment: .leading, spacing: 0) {
                ForEach(Array(diff.split(separator: "\n", omittingEmptySubsequences: false).prefix(400).enumerated()), id: \.offset) { _, line in
                    Text(line.isEmpty ? " " : String(line))
                        .font(.mono(12))
                        .foregroundStyle(color(line))
                        .padding(.horizontal, 6)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(background(line))
                }
            }
            .padding(.vertical, 4)
        }
        .frame(maxHeight: 260)
        .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(Palette.surface2))
        .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
    }

    private func color(_ l: Substring) -> Color {
        if l.hasPrefix("+++") || l.hasPrefix("---") || l.hasPrefix("@@") { return Palette.muted }
        if l.hasPrefix("+") { return Palette.ok }
        if l.hasPrefix("-") { return Palette.err }
        return Palette.text
    }

    private func background(_ l: Substring) -> Color {
        if l.hasPrefix("+++") || l.hasPrefix("---") { return .clear }
        if l.hasPrefix("+") { return Palette.okTint }
        if l.hasPrefix("-") { return Palette.errTint }
        return .clear
    }
}

/// A helper: a part of the job handed to a conversation of its own, its state and what it found.
struct HelperRow: View {
    let item: AgentItem

    var body: some View {
        HStack(alignment: .top, spacing: Grid.s) {
            Image(systemName: "person.2").foregroundStyle(Palette.muted).frame(width: 18)
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text(item.title.isEmpty ? "Helper" : item.title).font(.ui(14, weight: .medium)).foregroundStyle(Palette.text)
                    Spacer()
                    if item.state == "running" { ProgressView().controlSize(.mini) } else {
                        Badge(text: ToolState.label(item.state), color: ToolState.color(item.state))
                    }
                }
                if !item.summary.isEmpty { Text(item.summary).font(.ui(13)).foregroundStyle(Palette.muted) }
            }
        }
        .padding(12)
        .box(fill: Palette.surface2, edge: .clear, lifted: false)
    }
}

/// Why something did not happen, in plain words, in its level's colour.
struct NoticeRow: View {
    let item: AgentItem

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
