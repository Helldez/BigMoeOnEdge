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

/// The person's words, verbatim, in a bubble of the shell's raised ink at the trailing edge, the
/// images sent with them above. Held, it offers Copy and Edit and send again.
struct UserMessageView: View {
    @EnvironmentObject private var chat: ChatModel
    let item: ConversationItem
    let edit: (ConversationItem) -> Void

    var body: some View {
        HStack {
            Spacer(minLength: Grid.xl)
            VStack(alignment: .trailing, spacing: Grid.s) {
                if !item.images.isEmpty {
                    WrapRow {
                        ForEach(Array(item.images.enumerated()), id: \.offset) { _, url in DataURLThumbnail(dataURL: url, side: 96) }
                    }
                    .accessibilityLabel(item.images.count == 1 ? "1 image" : "\(item.images.count) images")
                }
                if !item.text.isEmpty {
                    Text(item.text)
                        .font(.ui(16))
                        .foregroundStyle(Palette.onShell)
                        .tint(Palette.onShell)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                        .background(BubbleShape().fill(Palette.shellHi))
                }
            }
            .contextMenu {
                if !item.text.isEmpty {
                    Button { UIPasteboard.general.string = item.text } label: { Label("Copy", systemImage: "doc.on.doc") }
                }
                Button { edit(item) } label: { Label("Edit and send again", systemImage: "pencil") }
                    .disabled(chat.running)
            }
        }
    }
}

/// The person's bubble: a 10-point corner everywhere but the bottom trailing one, 3 points, which
/// points at the edge the words came from. Drawn here since the system's uneven rectangle needs a
/// newer iOS than the app's oldest.
struct BubbleShape: Shape {
    var radius = Grid.control
    var tail: CGFloat = 3

    func path(in r: CGRect) -> Path {
        let big = min(radius, r.width / 2, r.height / 2), small = min(tail, big)
        var p = Path()
        p.move(to: CGPoint(x: r.minX + big, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX - big, y: r.minY))
        p.addArc(tangent1End: CGPoint(x: r.maxX, y: r.minY), tangent2End: CGPoint(x: r.maxX, y: r.minY + big), radius: big)
        p.addLine(to: CGPoint(x: r.maxX, y: r.maxY - small))
        p.addArc(tangent1End: CGPoint(x: r.maxX, y: r.maxY), tangent2End: CGPoint(x: r.maxX - small, y: r.maxY), radius: small)
        p.addLine(to: CGPoint(x: r.minX + big, y: r.maxY))
        p.addArc(tangent1End: CGPoint(x: r.minX, y: r.maxY), tangent2End: CGPoint(x: r.minX, y: r.maxY - big), radius: big)
        p.addLine(to: CGPoint(x: r.minX, y: r.minY + big))
        p.addArc(tangent1End: CGPoint(x: r.minX, y: r.minY), tangent2End: CGPoint(x: r.minX + big, y: r.minY), radius: big)
        p.closeSubpath()
        return p
    }
}

/// The model's answer on a sheet of its own, the column's whole width, with no header: Markdown,
/// its thinking folded above. While it streams, the rate it is written at under it; once it is
/// whole, Copy, Answer again on the last one, and the line of how it ran (docs/screens.md, "The
/// figures of an answer").
struct AssistantMessageView: View {
    @EnvironmentObject private var chat: ChatModel
    @EnvironmentObject private var engine: Engine
    let item: ConversationItem
    let answerAgain: Bool
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            if !item.reasoning.isEmpty { ThinkingView(text: item.reasoning, streaming: item.streaming && item.text.isEmpty) }
            if !item.text.isEmpty {
                AnswerMarkdown(source: item.text).foregroundStyle(Palette.text)
            } else if item.streaming && item.reasoning.isEmpty {
                Text("Reading your message…").font(.ui(15)).foregroundStyle(Palette.muted)
            }
            if item.streaming {
                // This turn's rate only: the last answer's average stays until the next token.
                if chat.running, engine.live.step > 0, !engine.live.finished {
                    Text(TurnFigures.liveLine(rate: engine.live.rate))
                        .font(.figure(13, weight: .medium)).foregroundStyle(Palette.muted)
                        .accessibilityLabel("Writing at \(TurnFigures.liveLine(rate: engine.live.rate))")
                }
            } else {
                HStack(spacing: Grid.m) {
                    if !item.text.isEmpty {
                        Button {
                            UIPasteboard.general.string = item.text
                            copied = true
                            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { copied = false }
                        } label: {
                            Label(copied ? "Copied" : "Copy", systemImage: copied ? "checkmark" : "doc.on.doc")
                        }
                        .accessibilityLabel("Copy")
                    }
                    if answerAgain {
                        Button { chat.answerAgain() } label: { Label("Answer again", systemImage: "arrow.clockwise") }
                            .disabled(chat.running)
                    }
                }
                .font(.ui(13, weight: .medium)).foregroundStyle(Palette.muted).buttonStyle(.borderless)
                if let f = chat.figures[item.id] { TurnFiguresView(summary: f.summary, nCtx: f.nCtx) }
            }
        }
        .padding(Grid.m)
        .frame(maxWidth: .infinity, alignment: .leading)
        .box()
    }
}

/// How a finished answer ran: one quiet line, `tg … · pp …`, that opens to each figure with what it
/// measures, the speed against what the plan expected, and the way to the plan and to Metrics. A
/// disclosure rather than a tooltip, so a finger and VoiceOver reach it.
struct TurnFiguresView: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var nav: Navigator
    let summary: TurnSummary
    let nCtx: Int
    @State private var open = false
    @State private var showMetrics = false

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Button {
                withAnimation(.easeInOut(duration: 0.15)) { open.toggle() }
            } label: {
                HStack(spacing: 6) {
                    Text(TurnFigures.line(summary)).font(.figure(13, weight: .medium))
                    Image(systemName: "chevron.down").font(.caption2.weight(.semibold))
                        .rotationEffect(.degrees(open ? 180 : 0))
                }
                .foregroundStyle(Palette.muted)
                .frame(minHeight: 28)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("How it ran: \(TurnFigures.line(summary))")
            .accessibilityAddTraits(open ? .isSelected : [])
            if open {
                VStack(alignment: .leading, spacing: 12) {
                    ForEach(TurnFigures.figures(summary, nCtx: nCtx, expectedTokS: expected)) { f in
                        VStack(alignment: .leading, spacing: 2) {
                            HStack(alignment: .firstTextBaseline, spacing: Grid.s) {
                                Text(f.label).font(.ui(13, weight: .bold)).foregroundStyle(Palette.text)
                                Spacer(minLength: Grid.s)
                                if !f.value.isEmpty, !f.valueOnOwnLine {
                                    Text(f.value).font(.figure(13, bold: true)).foregroundStyle(Palette.text)
                                }
                            }
                            // A long value on its own line, left-aligned, never a ragged column.
                            if f.valueOnOwnLine {
                                Text(f.value).font(.figure(13, bold: true)).foregroundStyle(Palette.text)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                            Text(f.hint).font(.ui(12)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
                        }
                        .accessibilityElement(children: .combine)
                    }
                }
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
                .box(lifted: false, radius: Grid.radius)
                HStack(spacing: Grid.m) {
                    Button("See the plan") { nav.open(.plan) }
                    Button("All figures") { showMetrics = true }
                }
                .font(.ui(13, weight: .medium)).foregroundStyle(Palette.accentInk).buttonStyle(.borderless)
            }
        }
        .sheet(isPresented: $showMetrics) {
            NavigationStack {
                MetricsView()
                    .background(Palette.bg.ignoresSafeArea())
                    .tint(Palette.accent)
                    .navigationTitle("Metrics")
                    .navigationBarTitleDisplayMode(.inline)
                    .toolbar {
                        ToolbarItem(placement: .confirmationAction) { Button("Done") { showMetrics = false } }
                    }
                    .shellBar()
            }
            .tint(Palette.onShell)
        }
    }

    /// The rate the plan expects for this model, when it predicts one.
    private var expected: Double? {
        guard let p = engine.plan, p.available, p.error.isEmpty else { return nil }
        return p.figures.expectedTokS
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
            Label(streaming ? "Reasoning…" : "Reasoning", systemImage: "brain").font(.ui(13, weight: .medium)).foregroundStyle(Palette.muted)
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
