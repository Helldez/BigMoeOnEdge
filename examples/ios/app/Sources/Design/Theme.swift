import SwiftUI
import UIKit

/// The visual language's tokens, the one place this app keeps them (docs/design.md, "Letterhead"):
/// ink on paper, light and dark following the system, each pair checked for 4.5:1 where it carries
/// words. Sheets (answers, cards, the composer) are `surface` on the `bg` desk; the navigation is
/// the `shell`'s ink. Words in the accent use `accentInk`; the accent itself marks what is not
/// text. The one primary button of a screen is `primary` with `onPrimary` words.
enum Palette {
    static let bg = dynamic(light: 0xF3F4F8, dark: 0x10121F)
    static let surface = dynamic(light: 0xFFFFFF, dark: 0x191C30)
    static let surface2 = dynamic(light: 0xECEEF4, dark: 0x20243C)
    static let line = dynamic(light: 0xDCDFEA, dark: 0x2C3150)
    /// A field's edge: the composer, a text field.
    static let lineStrong = dynamic(light: 0x6E7396, dark: 0x7D82A8)
    static let text = dynamic(light: 0x1B1F3A, dark: 0xE8E9F4)
    static let muted = dynamic(light: 0x555A7C, dark: 0xA9ADC9)
    static let accent = dynamic(light: 0x2F3A8F, dark: 0x8E9BFF)
    static let accentInk = dynamic(light: 0x2F3A8F, dark: 0xAAB4FF)
    static let accentTint = dynamic(light: 0xE6E8F4, dark: 0x23284A)
    static let primary = dynamic(light: 0x1B1F3A, dark: 0xE8E9F4)
    static let onPrimary = dynamic(light: 0xFFFFFF, dark: 0x10121F)
    /// Words on the accent's ink where it is a fill (a current step).
    static let onAccent = dynamic(light: 0xFFFFFF, dark: 0x10121F)
    /// The navigation: the top bar and the conversations list, the selected row in `shellHi`, which
    /// is also the person's own message bubble.
    static let shell = dynamic(light: 0x1B1F3A, dark: 0x0B0D18)
    static let shellHi = dynamic(light: 0x2C3160, dark: 0x23284A)
    static let onShell = dynamic(light: 0xFFFFFF, dark: 0xFFFFFF)
    static let onShellMuted = dynamic(light: 0xC7CAE0, dark: 0xB7BAD4)
    /// The logo's yellow: only the 3-point rule under the logo on the welcome sheet.
    static let seal = dynamic(light: 0xFFC53D, dark: 0xFFC53D)
    static let ok = dynamic(light: 0x157A3C, dark: 0x4ADE80)
    static let okTint = dynamic(light: 0xE6F4EA, dark: 0x12261B)
    static let warn = dynamic(light: 0x9A4A00, dark: 0xFBBF24)
    static let warnTint = dynamic(light: 0xFFF1D6, dark: 0x2A2111)
    static let err = dynamic(light: 0xB42318, dark: 0xF87171)
    static let errTint = dynamic(light: 0xFDECEA, dark: 0x2C1517)
    /// A sheet's shadow: soft, so the sheet lifts off the desk with no visible frame.
    static let shadow = dynamic(light: 0x1B1F3A, dark: 0x000000)

    /// Categorical colours for the parts of one whole (a memory bar's segments and their swatches):
    /// hues far apart, so neighbours are told apart by colour, not only by order. Part `i` takes
    /// `series[i % series.count]`; what is left over stays the empty track.
    static let series = [
        dynamic(light: 0x2F3A8F, dark: 0x8E9BFF), // ink
        dynamic(light: 0x0B8EA8, dark: 0x21E6FF), // teal
        dynamic(light: 0xB87400, dark: 0xFFC53D), // amber
        dynamic(light: 0x2E8B57, dark: 0x4ADE80), // green
        dynamic(light: 0xC2416E, dark: 0xFB7185), // rose
        dynamic(light: 0x6B6F88, dark: 0xA7ABC4), // slate
    ]

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

    /// A token fixed to one scheme: for a surface whose own traits say another (a bar told it is
    /// dark so its controls are light).
    static func resolved(_ c: Color, _ scheme: ColorScheme) -> Color {
        Color(UIColor(c).resolvedWithTraits(scheme))
    }

    private static func dynamic(light: UInt32, dark: UInt32) -> Color {
        Color(UIColor { $0.userInterfaceStyle == .dark ? uiColor(dark) : uiColor(light) })
    }

    static func uiColor(_ hex: UInt32) -> UIColor {
        UIColor(red: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
                blue: CGFloat(hex & 0xFF) / 255, alpha: 1)
    }
}

extension Preferences.Theme {
    /// What SwiftUI is asked for; nil follows the system.
    var colorScheme: ColorScheme? {
        switch self {
        case .system: return nil
        case .light: return .light
        case .dark: return .dark
        }
    }

    /// What the windows are told, so UIKit's bars and every dynamic colour above resolve in the
    /// chosen scheme; `.unspecified` hands the choice back to the system.
    var interfaceStyle: UIUserInterfaceStyle {
        switch self {
        case .system: return .unspecified
        case .light: return .light
        case .dark: return .dark
        }
    }
}

/// The grid: spacing is 8, 16, 24, 32. A sheet has a 6-point corner, a control (a button, a field,
/// the composer, a bubble) a 10-point one; a line is 1 point.
enum Grid {
    static let s: CGFloat = 8
    static let m: CGFloat = 16
    static let l: CGFloat = 24
    static let xl: CGFloat = 32
    static let radius: CGFloat = 6
    static let control: CGFloat = 10
    static let line: CGFloat = 1
}

/// The bundled faces by family, for whatever builds a font from a family and a weight itself (the
/// chat's Markdown renderer) rather than through `Font.ui` and `Font.mono` below.
enum Typeface {
    static let ui = "Hanken Grotesk"
    static let mono = "JetBrains Mono"
}

extension Font {
    /// Hanken Grotesk, bundled (Resources/Fonts), scaled with the person's text size from the nearest
    /// system style. Every word of the app is set in it.
    static func ui(_ size: CGFloat = 17, bold: Bool = false, weight: Font.Weight? = nil) -> Font {
        let w = weight ?? (bold ? .semibold : .regular)
        return .custom(textFace(w), size: size, relativeTo: style(for: size))
    }

    /// JetBrains Mono, bundled: anything a person may paste or a model wrote as code.
    static func mono(_ size: CGFloat = 14, bold: Bool = false) -> Font {
        .custom(bold ? "JetBrainsMono-Bold" : "JetBrainsMono-Regular", size: size, relativeTo: style(for: size))
    }

    /// Anything a person may paste: a command line, a path, a key.
    static let code = Font.mono(14)

    /// A figure the engine reports, with digits of one width so a live value does not jitter.
    static func figure(_ size: CGFloat = 15, bold: Bool = false, weight: Font.Weight? = nil) -> Font {
        ui(size, bold: bold, weight: weight).monospacedDigit()
    }

    fileprivate static func textFace(_ w: Font.Weight) -> String {
        switch w {
        case .bold, .heavy, .black: return "HankenGrotesk-Bold"
        case .semibold: return "HankenGrotesk-SemiBold"
        case .medium: return "HankenGrotesk-Medium"
        default: return "HankenGrotesk-Regular"
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
    /// Hanken Grotesk for the UIKit bars, which SwiftUI's navigation and tab bars read.
    static func ui(_ size: CGFloat, weight: UIFont.Weight = .regular) -> UIFont {
        let name = weight >= .bold ? "HankenGrotesk-Bold" : weight >= .semibold ? "HankenGrotesk-SemiBold"
            : weight >= .medium ? "HankenGrotesk-Medium" : "HankenGrotesk-Regular"
        return UIFont(name: name, size: size) ?? .systemFont(ofSize: size, weight: weight)
    }
}

private extension UIColor {
    func resolvedWithTraits(_ scheme: ColorScheme) -> UIColor {
        resolvedColor(with: UITraitCollection(userInterfaceStyle: scheme == .dark ? .dark : .light))
    }
}
