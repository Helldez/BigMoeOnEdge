import SwiftUI

/// The model's answer as rendered Markdown, with nothing in it that runs or fetches: the blocks a
/// chat answer uses (paragraphs, headings, lists, quotes, code, rules) laid out here, and each
/// line's inline marks (emphasis, code, links) by Foundation's own parser. No HTML is ever
/// interpreted and no image is loaded, so text that talks the model into writing one sends nothing
/// anywhere; a link goes where it says once the person taps it.
struct MarkdownText: View {
    let source: String

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ForEach(Array(MarkdownBlock.parse(source).enumerated()), id: \.offset) { _, block in
                view(block)
            }
        }
        .textSelection(.enabled)
    }

    @ViewBuilder
    private func view(_ b: MarkdownBlock) -> some View {
        switch b {
        case let .heading(level, text):
            Text(inline(text)).font(.system(size: level <= 1 ? 22 : level == 2 ? 19 : 17, weight: .bold))
        case let .paragraph(text):
            Text(inline(text)).fixedSize(horizontal: false, vertical: true)
        case let .list(ordered, items):
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(items.enumerated()), id: \.offset) { i, item in
                    HStack(alignment: .firstTextBaseline, spacing: Grid.s) {
                        Text(ordered ? "\(i + 1)." : "•").foregroundStyle(Palette.muted).monospacedDigit()
                        Text(inline(item)).fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
        case let .quote(text):
            Text(inline(text))
                .foregroundStyle(Palette.muted)
                .padding(.leading, 12)
                .overlay(alignment: .leading) { Rectangle().fill(Palette.accent2).frame(width: 3) }
        case let .code(text):
            ScrollView(.horizontal, showsIndicators: false) {
                Text(text).font(.code).foregroundStyle(Palette.text).padding(12)
            }
            .background(Palette.bg)
            .overlay(Rectangle().strokeBorder(Palette.border, lineWidth: Grid.edge))
        case .rule:
            Rectangle().fill(Palette.border).frame(height: Grid.edge)
        }
    }

    private func inline(_ s: String) -> AttributedString {
        var out = (try? AttributedString(markdown: s, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)))
            ?? AttributedString(s)
        for run in out.runs where run.link != nil {
            out[run.range].foregroundColor = Palette.accent
            out[run.range].underlineStyle = .single
        }
        return out
    }
}

/// The block structure of a chat answer: enough Markdown to read one, and nothing a model could
/// use to reach out of the transcript.
enum MarkdownBlock: Equatable {
    case heading(Int, String)
    case paragraph(String)
    case list(ordered: Bool, items: [String])
    case quote(String)
    case code(String)
    case rule

    static func parse(_ text: String) -> [MarkdownBlock] {
        var blocks: [MarkdownBlock] = []
        var paragraph: [String] = []
        var items: [String] = []
        var ordered = false
        var quote: [String] = []
        var code: [String]?

        func flush() {
            if !paragraph.isEmpty { blocks.append(.paragraph(paragraph.joined(separator: "\n"))) }
            if !items.isEmpty { blocks.append(.list(ordered: ordered, items: items)) }
            if !quote.isEmpty { blocks.append(.quote(quote.joined(separator: "\n"))) }
            paragraph = []
            items = []
            quote = []
        }

        for raw in text.components(separatedBy: "\n") {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if var open = code {
                if line.hasPrefix("```") {
                    blocks.append(.code(open.joined(separator: "\n")))
                    code = nil
                } else {
                    open.append(raw)
                    code = open
                }
                continue
            }
            if line.hasPrefix("```") {
                flush()
                code = []
            } else if line.isEmpty {
                flush()
            } else if line == "---" || line == "***" || line == "___" {
                flush()
                blocks.append(.rule)
            } else if let h = heading(line) {
                flush()
                blocks.append(h)
            } else if line.hasPrefix(">") {
                if !paragraph.isEmpty || !items.isEmpty { flush() }
                quote.append(String(line.dropFirst()).trimmingCharacters(in: .whitespaces))
            } else if let (isOrdered, item) = listItem(line) {
                if !paragraph.isEmpty || !quote.isEmpty || (!items.isEmpty && isOrdered != ordered) { flush() }
                ordered = isOrdered
                items.append(item)
            } else if !items.isEmpty && raw.hasPrefix("  ") {
                items[items.count - 1] += " " + line // a list item's continuation line
            } else {
                if !items.isEmpty || !quote.isEmpty { flush() }
                paragraph.append(line)
            }
        }
        // An answer still streaming may have a code fence open: what came so far is code.
        if let open = code { blocks.append(.code(open.joined(separator: "\n"))) }
        flush()
        return blocks
    }

    private static func heading(_ line: String) -> MarkdownBlock? {
        let level = line.prefix { $0 == "#" }.count
        guard (1...6).contains(level), line.dropFirst(level).first == " " else { return nil }
        return .heading(level, String(line.dropFirst(level + 1)))
    }

    private static func listItem(_ line: String) -> (Bool, String)? {
        for marker in ["- ", "* ", "+ "] where line.hasPrefix(marker) { return (false, String(line.dropFirst(2))) }
        let digits = line.prefix { $0.isNumber }
        if !digits.isEmpty, digits.count <= 3 {
            let rest = line.dropFirst(digits.count)
            if rest.hasPrefix(". ") || rest.hasPrefix(") ") { return (true, String(rest.dropFirst(2))) }
        }
        return nil
    }
}
