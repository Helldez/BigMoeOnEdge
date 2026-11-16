import SwiftUI

/// A box of the language. A sheet (a card, an answer) is the surface with a 6-point corner, lifted
/// off the desk by a soft shadow and no visible frame; an inner box (a field, a row) stays flat
/// with a 1-point line.
struct Box: ViewModifier {
    var fill = Palette.surface
    var edge: Color?
    var lifted = true
    var radius = Grid.radius

    func body(content: Content) -> some View {
        let shape = RoundedRectangle(cornerRadius: radius, style: .continuous)
        content
            .background(shape.fill(fill))
            .overlay(shape.strokeBorder(edge ?? (lifted ? .clear : Palette.line), lineWidth: Grid.line))
            .sheetShadow(lifted)
    }
}

extension View {
    func box(fill: Color = Palette.surface, edge: Color? = nil, lifted: Bool = true, radius: CGFloat = Grid.radius) -> some View {
        modifier(Box(fill: fill, edge: edge, lifted: lifted, radius: radius))
    }

    /// The sheets' soft shadow: a hairline close under the edge and a wide, faint one below it.
    @ViewBuilder
    func sheetShadow(_ on: Bool = true) -> some View {
        if on {
            shadow(color: Palette.shadow.opacity(0.08), radius: 1, x: 0, y: 1)
                .shadow(color: Palette.shadow.opacity(0.08), radius: 12, x: 0, y: 6)
        } else {
            self
        }
    }

    /// A card: the padding of the grid inside the box.
    func card(padding: CGFloat = Grid.m) -> some View {
        self.padding(padding).frame(maxWidth: .infinity, alignment: .leading).box()
    }
}

/// A button of the language: the primary one (one a screen) filled with the ink, a secondary on the
/// second surface, a destructive one in the error's tint. Pressed, it dims: the system's feedback.
struct AppButtonStyle: ButtonStyle {
    enum Kind { case primary, secondary, destructive }
    var kind: Kind = .secondary
    @Environment(\.isEnabled) private var enabled

    func makeBody(configuration: Configuration) -> some View {
        let shape = RoundedRectangle(cornerRadius: Grid.control, style: .continuous)
        return configuration.label
            .font(.ui(15, weight: .semibold))
            .lineLimit(1)
            .padding(.horizontal, 14)
            .padding(.vertical, 9)
            .foregroundStyle(foreground)
            .background(shape.fill(fill))
            .overlay(shape.strokeBorder(kind == .secondary ? Palette.line : .clear, lineWidth: Grid.line))
            .opacity(enabled ? (configuration.isPressed ? 0.7 : 1) : 0.45)
            .contentShape(shape)
    }

    private var fill: Color {
        switch kind {
        case .primary: return Palette.primary
        case .secondary: return Palette.surface2
        case .destructive: return Palette.errTint
        }
    }

    private var foreground: Color {
        switch kind {
        case .primary: return Palette.onPrimary
        case .secondary: return Palette.text
        case .destructive: return Palette.err
        }
    }
}

extension ButtonStyle where Self == AppButtonStyle {
    static var appSecondary: AppButtonStyle { AppButtonStyle() }
    static var appPrimary: AppButtonStyle { AppButtonStyle(kind: .primary) }
    static var appDestructive: AppButtonStyle { AppButtonStyle(kind: .destructive) }
}

/// A short label in a tinted capsule: a state, a source, a level.
struct Badge: View {
    let text: String
    var color: Color = Palette.muted

    var body: some View {
        Text(text)
            .font(.ui(12, weight: .medium))
            // The accent is too light for words on its tint in the light scheme: its ink says them.
            .foregroundStyle(color == Palette.accent ? Palette.accentInk : color)
            .padding(.horizontal, 8)
            .padding(.vertical, 2)
            .background(Capsule().fill(Palette.tint(color)))
    }
}

/// A meter: the second surface as the track, the fill, rounded ends, and where there is one a
/// mark at a second figure (what a plan credited, beside what was measured).
struct Meter: View {
    let fraction: Double
    var color: Color = Palette.accent
    var height: CGFloat = 8
    var mark: Double?

    var body: some View {
        GeometryReader { g in
            ZStack(alignment: .leading) {
                Capsule().fill(Palette.surface2)
                Capsule().fill(color).frame(width: g.size.width * min(1, max(0, fraction)))
                if let mark {
                    Rectangle().fill(Palette.text).frame(width: 2)
                        .offset(x: max(0, g.size.width * min(1, max(0, mark)) - 2))
                }
            }
        }
        .frame(height: height)
        .clipShape(Capsule())
    }
}

/// A section title.
struct SectionTitle: View {
    let text: String
    var trailing: String?

    init(_ text: String, trailing: String? = nil) {
        self.text = text
        self.trailing = trailing
    }

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(text).font(.ui(17, weight: .semibold)).foregroundStyle(Palette.text)
            Spacer()
            if let trailing { Text(trailing).font(.figure(13)).foregroundStyle(Palette.muted) }
        }
    }
}

/// A banner: a message on a semantic colour's tint, with an action when there is one to take.
struct Banner<Actions: View>: View {
    let text: String
    var color: Color = Palette.warn
    @ViewBuilder var actions: Actions

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Text(text).font(.ui(15)).foregroundStyle(Palette.text).fixedSize(horizontal: false, vertical: true)
            actions
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: Grid.radius, style: .continuous).fill(Palette.tint(color)))
        .overlay(alignment: .leading) {
            RoundedRectangle(cornerRadius: 2).fill(color).frame(width: 3).padding(.vertical, 8)
        }
    }
}

extension Banner where Actions == EmptyView {
    init(text: String, color: Color = Palette.warn) {
        self.init(text: text, color: color) { EmptyView() }
    }
}

/// A text field of the language: the surface, the strong line of a field's edge, the accent while
/// focused.
struct AppField: View {
    let placeholder: String
    @Binding var text: String
    var mono = false
    var axis: Axis = .horizontal
    @FocusState private var focused: Bool

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: Grid.control, style: .continuous)
        TextField(placeholder, text: $text, axis: axis)
            .font(mono ? .code : .ui(17))
            .foregroundStyle(Palette.text)
            .tint(Palette.accent)
            .focused($focused)
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .background(shape.fill(Palette.surface))
            .overlay(shape.strokeBorder(focused ? Palette.accent : Palette.lineStrong, lineWidth: focused ? 2 : Grid.line))
    }
}

/// A figure and its name, as the engine reports it.
struct Figure: View {
    let value: String
    let label: String

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(value).font(.figure(20, bold: true)).foregroundStyle(Palette.text)
            Text(label).font(.ui(12)).foregroundStyle(Palette.muted)
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
                withAnimation(.easeInOut(duration: 0.15)) {
                    if let isOpen { isOpen.wrappedValue = !opened } else { shown = !opened }
                }
            } label: {
                HStack(spacing: 10) {
                    Image(systemName: "chevron.right").font(.caption.bold())
                        .rotationEffect(.degrees(opened ? 90 : 0))
                        .foregroundStyle(Palette.muted).frame(width: 14)
                    Text(title).font(.ui(16, weight: .semibold)).foregroundStyle(Palette.text).multilineTextAlignment(.leading)
                    Spacer()
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if !hint.isEmpty { Text(hint).font(.ui(12)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true) }
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
                    .font(.ui(13, weight: i == current ? .semibold : .regular))
                    .foregroundStyle(i == current ? Palette.onAccent : i < current ? Palette.ok : Palette.muted)
                    .padding(.horizontal, 8).padding(.vertical, 3)
                    .background(Capsule().fill(i == current ? Palette.accentInk : i < current ? Palette.okTint : Palette.surface2))
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
        }
        .background(Palette.bg.ignoresSafeArea())
        .scrollDismissesKeyboard(.interactively)
    }
}

/// A dot in a state's colour, beside a line that names the state.
struct StateDot: View {
    let color: Color

    var body: some View { Circle().fill(color).frame(width: 8, height: 8) }
}
