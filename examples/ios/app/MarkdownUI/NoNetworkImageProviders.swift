import SwiftUI

// Compiled into the vendored MarkdownUI module in place of its DefaultImageProvider.swift and
// DefaultInlineImageProvider.swift, which load every image a Markdown source names from the network
// through NetworkImage. A chat answer is text a model wrote, and text can be talked into naming a
// URL, so rendering one must fetch nothing: the two defaults keep their names (the module's own
// environment defaults and deprecated initializers refer to them) but draw nothing. The image's
// alt text stays its accessibility label (MarkdownUI's ImageView sets it), and a linked image is
// still a link that opens only on a tap.

/// The block image provider every Markdown view gets unless told otherwise: nothing is loaded and
/// nothing is drawn.
public struct DefaultImageProvider: ImageProvider {
    public init() {}

    public func makeImage(url: URL?) -> some View {
        EmptyView()
    }
}

extension ImageProvider where Self == DefaultImageProvider {
    public static var `default`: Self { .init() }
}

/// The inline image provider every Markdown view gets unless told otherwise: every image fails
/// without a request, and MarkdownUI then leaves it out of the line.
public struct DefaultInlineImageProvider: InlineImageProvider {
    public init() {}

    public func image(with url: URL, label: String) async throws -> Image {
        throw URLError(.resourceUnavailable)
    }
}

extension InlineImageProvider where Self == DefaultInlineImageProvider {
    public static var `default`: Self { .init() }
}
