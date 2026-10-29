import MarkdownUI
import SwiftUI

/// The model's answer as rendered Markdown, GitHub-flavoured (tables, task lists, strikethrough,
/// autolinks), by MarkdownUI over cmark-gfm (third_party/), set in this app's tokens. Nothing in it
/// runs or fetches, so text that talks the model into writing HTML or an image sends nothing
/// anywhere: raw HTML is shown as the text it is, the image providers MarkdownUI is built with load
/// nothing (app/MarkdownUI/NoNetworkImageProviders.swift), and a link goes where it says once the
/// person taps it.
struct AnswerMarkdown: View {
    let source: String

    var body: some View {
        Markdown(source)
            .markdownTheme(.answer)
            .textSelection(.enabled)
    }
}

extension MarkdownUI.Theme {
    /// docs/design.md in MarkdownUI's terms: Inter at the chat's 16 points, JetBrains Mono for code
    /// on the second surface, headings in Inter Bold at the sizes the transcript used before, rules
    /// and table borders in the line token.
    static let answer = Theme()
        .text {
            FontFamily(.custom(Typeface.ui))
            FontSize(16)
            ForegroundColor(Palette.text)
        }
        .code {
            FontFamily(.custom(Typeface.mono))
            FontSize(.em(0.875))
            BackgroundColor(Palette.surface2)
        }
        .strong { FontWeight(.semibold) }
        .link {
            ForegroundColor(Palette.accentInk)
            UnderlineStyle(.single)
        }
        .heading1 { heading($0, scale: 22.0 / 16) }
        .heading2 { heading($0, scale: 19.0 / 16) }
        .heading3 { heading($0, scale: 17.0 / 16) }
        .heading4 { heading($0, scale: 1) }
        .heading5 { heading($0, scale: 1) }
        .heading6 { heading($0, scale: 1) }
        .paragraph { configuration in
            configuration.label
                .fixedSize(horizontal: false, vertical: true)
                .relativeLineSpacing(.em(0.2))
                .markdownMargin(top: 0, bottom: Grid.m)
        }
        .listItem { configuration in
            configuration.label.markdownMargin(top: .em(0.25))
        }
        .blockquote { configuration in
            configuration.label
                .markdownTextStyle { ForegroundColor(Palette.muted) }
                .padding(.leading, 12)
                .overlay(alignment: .leading) { Rectangle().fill(Palette.accentInk).frame(width: 3) }
                .fixedSize(horizontal: false, vertical: true)
                .markdownMargin(top: 0, bottom: Grid.m)
        }
        .codeBlock { configuration in
            // A code line is never wrapped: it scrolls, as it does in an editor.
            ScrollView(.horizontal, showsIndicators: false) {
                configuration.label
                    .fixedSize(horizontal: false, vertical: true)
                    .relativeLineSpacing(.em(0.2))
                    .markdownTextStyle {
                        FontFamily(.custom(Typeface.mono))
                        FontSize(.em(0.875))
                    }
                    .padding(12)
            }
            .background(RoundedRectangle(cornerRadius: Grid.radius, style: .continuous).fill(Palette.surface2))
            .markdownMargin(top: 0, bottom: Grid.m)
        }
        .table { configuration in
            // A table wider than the transcript scrolls sideways rather than squeezing its columns
            // into a word per line.
            ScrollView(.horizontal, showsIndicators: false) {
                configuration.label
                    .fixedSize(horizontal: false, vertical: true)
                    .markdownTableBorderStyle(.init(color: Palette.line, width: Grid.line))
                    .markdownTableBackgroundStyle(.alternatingRows(Color.clear, Palette.surface2, header: Palette.surface2))
            }
            .markdownMargin(top: 0, bottom: Grid.m)
        }
        .tableCell { configuration in
            configuration.label
                .markdownTextStyle {
                    if configuration.row == 0 { FontWeight(.semibold) }
                    BackgroundColor(nil)
                }
                .fixedSize(horizontal: false, vertical: true)
                .padding(.vertical, 6)
                .padding(.horizontal, 12)
        }
        .thematicBreak {
            Rectangle().fill(Palette.line).frame(height: Grid.line)
                .markdownMargin(top: Grid.s, bottom: Grid.m)
        }

    private static func heading(_ configuration: BlockConfiguration, scale: CGFloat) -> some View {
        configuration.label
            .relativeLineSpacing(.em(0.125))
            .markdownMargin(top: Grid.s, bottom: Grid.s)
            .markdownTextStyle {
                FontWeight(.bold)
                FontSize(.em(scale))
            }
    }
}
