import SwiftUI

/// A box of the language: the surface, a 2-point edge, square corners and, for a card, the hard
/// shadow 4 points right and down with no blur.
struct PixelBox: ViewModifier {
    var fill = Palette.surface
    var edge = Palette.border
    var shadow = true

    func body(content: Content) -> some View {
        content
            .background(fill)
            .overlay(Rectangle().strokeBorder(edge, lineWidth: Grid.edge))
            .background(
                Rectangle().fill(shadow ? Palette.shadow : .clear).offset(x: Grid.shadow, y: Grid.shadow))
    }
}

extension View {
    func pixelBox(fill: Color = Palette.surface, edge: Color = Palette.border, shadow: Bool = true) -> some View {
        modifier(PixelBox(fill: fill, edge: edge, shadow: shadow))
    }

    /// A card: the padding of the grid inside the box.
    func card(padding: CGFloat = Grid.m) -> some View {
        self.padding(padding).frame(maxWidth: .infinity, alignment: .leading).pixelBox()
    }
}

/// A button of the language. The primary one is the accent with the shadow, pressed 2 points into
/// it; a secondary has the edge and no shadow; a destructive one says so in the error colour.
struct PixelButtonStyle: ButtonStyle {
    enum Kind { case primary, secondary, destructive }
    var kind: Kind = .secondary
    @Environment(\.isEnabled) private var enabled

    func makeBody(configuration: Configuration) -> some View {
        let pressed = configuration.isPressed
        return configuration.label
            .font(.pixel(15, bold: true))
            .lineLimit(1)
            .padding(.horizontal, 12)
            .padding(.vertical, Grid.s)
            .foregroundStyle(foreground)
            .background(fill)
            .overlay(Rectangle().strokeBorder(edge, lineWidth: Grid.edge))
            .background(Rectangle().fill(kind == .primary && enabled ? Palette.shadow : .clear)
                .offset(x: pressed ? 2 : Grid.shadow, y: pressed ? 2 : Grid.shadow))
            .offset(x: kind == .primary && pressed ? 2 : 0, y: kind == .primary && pressed ? 2 : 0)
            .opacity(enabled ? 1 : 0.45)
            .contentShape(Rectangle())
    }

    private var fill: Color {
        switch kind {
        case .primary: return Palette.accent
        case .secondary: return Palette.surface
        case .destructive: return Palette.tint(Palette.err)
        }
    }

    private var foreground: Color {
        switch kind {
        case .primary: return Palette.accentText
        case .secondary: return Palette.text
        case .destructive: return Palette.err
        }
    }

    private var edge: Color {
        switch kind {
        case .primary: return Palette.accent
        case .secondary: return Palette.border
        case .destructive: return Palette.err
        }
    }
}

extension ButtonStyle where Self == PixelButtonStyle {
    static var pixel: PixelButtonStyle { PixelButtonStyle() }
    static var pixelPrimary: PixelButtonStyle { PixelButtonStyle(kind: .primary) }
    static var pixelDestructive: PixelButtonStyle { PixelButtonStyle(kind: .destructive) }
}

/// A short label in a tinted box: a state, a source, a level.
struct Badge: View {
    let text: String
    var color: Color = Palette.muted

    var body: some View {
        Text(text.uppercased())
            .font(.pixel(13))
            .foregroundStyle(color)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(Palette.tint(color))
            .overlay(Rectangle().strokeBorder(color, lineWidth: 1))
    }
}

/// A flat meter: the `surface-2` track, the fill, a 2-point edge, and where there is one a 2-point
/// mark at a second figure (what a plan credited, beside what was measured).
struct Meter: View {
    let fraction: Double
    var color: Color = Palette.accent
    var height: CGFloat = 12
    var mark: Double?

    var body: some View {
        GeometryReader { g in
            ZStack(alignment: .leading) {
                Rectangle().fill(Palette.surface2)
                Rectangle().fill(color).frame(width: g.size.width * min(1, max(0, fraction)))
                if let mark {
                    Rectangle().fill(Palette.text).frame(width: Grid.edge)
                        .offset(x: max(0, g.size.width * min(1, max(0, mark)) - Grid.edge))
                }
            }
        }
        .frame(height: height)
        .overlay(Rectangle().strokeBorder(Palette.border, lineWidth: Grid.edge))
    }
}

/// A section title in the product's voice.
struct SectionTitle: View {
    let text: String
    var trailing: String?

    init(_ text: String, trailing: String? = nil) {
        self.text = text
        self.trailing = trailing
    }

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(text).font(.pixel(18, bold: true)).foregroundStyle(Palette.accent2)
            Spacer()
            if let trailing { Text(trailing).font(.pixel(13)).foregroundStyle(Palette.muted) }
        }
    }
}

/// A banner: a message in a semantic colour, with an action when there is one to take.
struct Banner<Actions: View>: View {
    let text: String
    var color: Color = Palette.warn
    @ViewBuilder var actions: Actions

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Text(text).font(.subheadline).foregroundStyle(Palette.text).fixedSize(horizontal: false, vertical: true)
            actions
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Palette.tint(color))
        .overlay(Rectangle().strokeBorder(color, lineWidth: Grid.edge))
    }
}

extension Banner where Actions == EmptyView {
    init(text: String, color: Color = Palette.warn) {
        self.init(text: text, color: color) { EmptyView() }
    }
}

/// A text field of the language: the surface, the edge, the accent edge while focused.
struct PixelField: View {
    let placeholder: String
    @Binding var text: String
    var mono = false
    var axis: Axis = .horizontal
    @FocusState private var focused: Bool

    var body: some View {
        TextField(placeholder, text: $text, axis: axis)
            .font(mono ? .code : .body)
            .foregroundStyle(Palette.text)
            .tint(Palette.accent)
            .focused($focused)
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .background(Palette.surface)
            .overlay(Rectangle().strokeBorder(focused ? Palette.accent : Palette.border, lineWidth: Grid.edge))
    }
}

/// A figure and its name, as the engine reports it.
struct Figure: View {
    let value: String
    let label: String

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(value).font(.pixel(20, bold: true)).foregroundStyle(Palette.text).monospacedDigit()
            Text(label).font(.caption).foregroundStyle(Palette.muted)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// A card that opens on a tap: a title, a line on what it holds, and its rows. Open or shut on its
/// own, or by `isOpen` where a link has to open it.
struct Fold<Content: View>: View {
    let title: String
    var hint = ""
    var open = false
    var isOpen: Binding<Bool>?
    var boxed = true
    @ViewBuilder var content: Content
    @State private var shown: Bool?

    var body: some View {
        let opened = isOpen?.wrappedValue ?? shown ?? open
        let column = VStack(alignment: .leading, spacing: Grid.s) {
            Button {
                if let isOpen { isOpen.wrappedValue = !opened } else { shown = !opened }
            } label: {
                HStack(spacing: 10) {
                    Image(systemName: opened ? "chevron.down" : "chevron.right").font(.caption.bold())
                        .foregroundStyle(Palette.muted).frame(width: 14)
                    Text(title).font(.pixel(16, bold: true)).foregroundStyle(Palette.text).multilineTextAlignment(.leading)
                    Spacer()
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if !hint.isEmpty { Text(hint).font(.caption).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true) }
            if opened { content }
        }
        if boxed { column.card() } else { column.frame(maxWidth: .infinity, alignment: .leading) }
    }
}

/// The steps of a guided flow in one line, the current one marked, the ones behind it done.
struct Steps: View {
    let labels: [String]
    let current: Int

    var body: some View {
        HStack(spacing: 6) {
            ForEach(labels.indices, id: \.self) { i in
                Text("\(i + 1) \(labels[i])")
                    .font(.pixel(13, bold: i == current))
                    .foregroundStyle(i == current ? Palette.accentText : i < current ? Palette.ok : Palette.muted)
                    .padding(.horizontal, 6).padding(.vertical, 2)
                    .background(i == current ? Palette.accent : .clear)
                    .overlay(Rectangle().strokeBorder(i < current ? Palette.ok : Palette.border, lineWidth: 1))
                    .accessibilityAddTraits(i == current ? .isSelected : [])
            }
        }
    }
}

/// A screen's body: the page colour and the grid's gutters, scrolled.
struct Page<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Grid.l) { content }
                .padding(Grid.m)
                .padding(.trailing, Grid.shadow)
        }
        .background(Palette.bg.ignoresSafeArea())
        .scrollDismissesKeyboard(.interactively)
    }
}
