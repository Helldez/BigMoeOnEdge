import SwiftUI

/// The logo: the 19 by 19 pixel chip, drawn from the project's own `docs/assets/logo/icon.svg`
/// (bundled with the app), so the mark here is the mark everywhere and is never redrawn by hand.
struct Logo: View {
    var size: CGFloat = 24

    var body: some View {
        Canvas { ctx, canvas in
            let unit = canvas.width / LogoPixels.shared.side
            for r in LogoPixels.shared.rects {
                ctx.fill(Path(CGRect(x: r.x * unit, y: r.y * unit, width: r.w * unit, height: r.h * unit)), with: .color(r.color))
            }
        }
        .frame(width: size, height: size)
        .accessibilityLabel("BigMoeOnEdge")
    }
}

/// The logo's pixels, read once from the SVG: its `viewBox` and its `rect` elements.
struct LogoPixels {
    struct Pixel {
        let x, y, w, h: CGFloat
        let color: Color
    }

    let side: CGFloat
    let rects: [Pixel]

    static let shared = LogoPixels(url: Bundle.main.url(forResource: "icon", withExtension: "svg"))

    init(url: URL?) {
        let svg = url.flatMap { try? String(contentsOf: $0, encoding: .utf8) } ?? ""
        let box = Self.attribute("viewBox", in: svg)?.split(separator: " ").compactMap { Double($0) } ?? []
        side = box.count == 4 ? CGFloat(box[2]) : 19
        var out: [Pixel] = []
        var rest = svg[...]
        while let start = rest.range(of: "<rect"), let end = rest[start.upperBound...].range(of: "/>") {
            let tag = String(rest[start.upperBound..<end.lowerBound])
            rest = rest[end.upperBound...]
            let n = { (k: String) in CGFloat(Double(Self.attribute(k, in: tag) ?? "") ?? 0) }
            guard let fill = Self.attribute("fill", in: tag), fill.hasPrefix("#"), let hex = UInt32(fill.dropFirst(), radix: 16) else {
                continue
            }
            out.append(Pixel(x: n("x"), y: n("y"), w: n("width"), h: n("height"), color: Color(Palette.uiColor(hex))))
        }
        rects = out
    }

    private static func attribute(_ name: String, in text: String) -> String? {
        guard let r = text.range(of: " \(name)=\"") ?? text.range(of: "\n\(name)=\"") else { return nil }
        let after = text[r.upperBound...]
        guard let close = after.firstIndex(of: "\"") else { return nil }
        return String(after[..<close])
    }
}
