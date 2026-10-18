import SwiftUI
import UIKit

/// The visual language (docs/design.md) as SwiftUI reads it: the logo's palette as colours that
/// follow the system's appearance, Pixelify Sans for the product's own voice (titles, labels,
/// figures), the system face for prose, the system mono for what a person may paste, an 8-point
/// grid, square corners, 2-point edges and a hard shadow.
enum Palette {
    static let bg = dynamic(dark: 0x0A0512, light: 0xF2ECFF)
    static let surface = dynamic(dark: 0x160B2A, light: 0xFFFFFF)
    static let surface2 = dynamic(dark: 0x2A1150, light: 0xE6DCFF)
    static let border = dynamic(dark: 0x3B2A63, light: 0x6C5AA8)
    static let text = dynamic(dark: 0xF2ECFF, light: 0x0A0512)
    static let muted = dynamic(dark: 0xB9A9E6, light: 0x4B3A7A)
    static let accent = dynamic(dark: 0x21E6FF, light: 0x0FA8C2)
    static let accentText = dynamic(dark: 0x0A0512, light: 0xFFFFFF)
    static let accent2 = dynamic(dark: 0x6C5AA8, light: 0x6C5AA8)
    static let warn = dynamic(dark: 0xFFC53D, light: 0x8A5A00)
    static let ok = dynamic(dark: 0x7CE0B0, light: 0x1D7A46)
    static let err = dynamic(dark: 0xFF6B8B, light: 0xB42318)
    static let shadow = dynamic(dark: 0x000000, light: 0x0A0512)

    /// A semantic colour's tint for banners and badges: the colour at 18% over the surface.
    static func tint(_ c: Color) -> Color { c.opacity(0.18) }

    private static func dynamic(dark: UInt32, light: UInt32) -> Color {
        Color(UIColor { $0.userInterfaceStyle == .light ? uiColor(light) : uiColor(dark) })
    }

    static func uiColor(_ hex: UInt32) -> UIColor {
        UIColor(red: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
                blue: CGFloat(hex & 0xFF) / 255, alpha: 1)
    }
}

/// The grid: spacing is 8, 16, 24, 32.
enum Grid {
    static let s: CGFloat = 8
    static let m: CGFloat = 16
    static let l: CGFloat = 24
    static let xl: CGFloat = 32
    static let edge: CGFloat = 2
    static let shadow: CGFloat = 4
}

extension Font {
    /// The product's own voice: titles, screen names, headings, badges and every figure the engine
    /// reports. Never below 13 points.
    static func pixel(_ size: CGFloat = 15, bold: Bool = false) -> Font {
        .custom("Pixelify Sans", size: max(13, size)).weight(bold ? .bold : .regular)
    }

    /// Anything a person may paste: a command line, a path, a key.
    static let code = Font.system(.footnote, design: .monospaced)
}
