import SwiftUI
import UIKit

/// The visual language's tokens, the one place this app keeps them (docs/design.md): slate and
/// cyan, light and dark following the system, each pair checked for 4.5:1 where it carries words.
/// The accent itself is too light for words on white in the light scheme, so words in the accent
/// and the primary button's fill use `accentInk`, and the accent marks what is not text.
enum Palette {
    static let bg = dynamic(light: 0xF6F8F9, dark: 0x0B1014)
    static let surface = dynamic(light: 0xFFFFFF, dark: 0x131A20)
    static let surface2 = dynamic(light: 0xEEF2F4, dark: 0x1A232B)
    static let line = dynamic(light: 0xDFE5E9, dark: 0x222C34)
    static let text = dynamic(light: 0x111820, dark: 0xE6EDF2)
    static let muted = dynamic(light: 0x56636E, dark: 0x93A1AC)
    static let accent = dynamic(light: 0x0891B2, dark: 0x22B8D6)
    static let accentInk = dynamic(light: 0x0E7490, dark: 0x22B8D6)
    static let onAccent = dynamic(light: 0xFFFFFF, dark: 0x0B1014)
    static let accentTint = dynamic(light: 0xE1F2F6, dark: 0x15333D)
    static let ok = dynamic(light: 0x166534, dark: 0x34D399)
    static let okTint = dynamic(light: 0xE3EDE7, dark: 0x183833)
    static let warn = dynamic(light: 0x92400E, dark: 0xFBBF24)
    static let warnTint = dynamic(light: 0xF2E8E2, dark: 0x383421)
    static let err = dynamic(light: 0xB91C1C, dark: 0xF87171)
    static let errTint = dynamic(light: 0xF7E4E4, dark: 0x38282D)

    /// A semantic colour's tint, for banners and badges: the named tint where there is one.
    static func tint(_ c: Color) -> Color {
        switch c {
        case ok: return okTint
        case warn: return warnTint
        case err: return errTint
        case accent, accentInk: return accentTint
        default: return c.opacity(0.14)
        }
    }

    private static func dynamic(light: UInt32, dark: UInt32) -> Color {
        Color(UIColor { $0.userInterfaceStyle == .dark ? uiColor(dark) : uiColor(light) })
    }

    static func uiColor(_ hex: UInt32) -> UIColor {
        UIColor(red: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
                blue: CGFloat(hex & 0xFF) / 255, alpha: 1)
    }
}

/// The grid: spacing is 8, 16, 24, 32; every box has an 8-point corner and a 1-point line.
enum Grid {
    static let s: CGFloat = 8
    static let m: CGFloat = 16
    static let l: CGFloat = 24
    static let xl: CGFloat = 32
    static let radius: CGFloat = 8
    static let line: CGFloat = 1
}

extension Font {
    /// Inter, bundled (Resources/Fonts), scaled with the person's text size from the nearest
    /// system style. Every word of the app is set in it.
    static func ui(_ size: CGFloat = 17, bold: Bool = false, weight: Font.Weight? = nil) -> Font {
        let w = weight ?? (bold ? .semibold : .regular)
        return .custom(interName(w), size: size, relativeTo: style(for: size))
    }

    /// JetBrains Mono, bundled: anything a person may paste or a model wrote as code.
    static func mono(_ size: CGFloat = 14, bold: Bool = false) -> Font {
        .custom(bold ? "JetBrainsMono-Bold" : "JetBrainsMono-Regular", size: size, relativeTo: style(for: size))
    }

    /// Anything a person may paste: a command line, a path, a key.
    static let code = Font.mono(14)

    /// A figure the engine reports, with digits of one width so a live value does not jitter.
    static func figure(_ size: CGFloat = 15, bold: Bool = false) -> Font { ui(size, bold: bold).monospacedDigit() }

    private static func interName(_ w: Font.Weight) -> String {
        switch w {
        case .bold, .heavy, .black: return "Inter-Bold"
        case .semibold: return "Inter-SemiBold"
        case .medium: return "Inter-Medium"
        default: return "Inter-Regular"
        }
    }

    private static func style(for size: CGFloat) -> Font.TextStyle {
        switch size {
        case ..<12: return .caption2
        case ..<13: return .caption
        case ..<14: return .footnote
        case ..<16: return .subheadline
        case ..<18: return .body
        case ..<21: return .title3
        case ..<26: return .title2
        default: return .title
        }
    }
}

extension UIFont {
    /// Inter for the UIKit bars, which SwiftUI's navigation and tab bars read.
    static func ui(_ size: CGFloat, weight: UIFont.Weight = .regular) -> UIFont {
        let name = weight >= .bold ? "Inter-Bold" : weight >= .semibold ? "Inter-SemiBold" : weight >= .medium ? "Inter-Medium"
            : "Inter-Regular"
        return UIFont(name: name, size: size) ?? .systemFont(ofSize: size, weight: weight)
    }
}
