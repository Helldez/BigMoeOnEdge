// Renders the app icon from the logo: the chip of docs/assets/logo/icon.svg on the `bg` colour
// (docs/design.md, "The logo"), pixel for pixel, at the one size the asset catalog needs.
//
//   xcrun swift examples/ios/app/Resources/make-icon.swift docs/assets/logo/icon.svg \
//       examples/ios/app/Resources/Assets.xcassets/AppIcon.appiconset/icon-1024.png
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let args = CommandLine.arguments
let svg = try String(contentsOfFile: args[1], encoding: .utf8)
let side = 1024
let grid = 19
let unit = 48 // 19 x 48 = 912: the chip with a margin of the background around it
let margin = (side - grid * unit) / 2

func attribute(_ name: String, _ tag: Substring) -> String? {
    guard let r = tag.range(of: " \(name)=\"") else { return nil }
    let rest = tag[r.upperBound...]
    return rest.firstIndex(of: "\"").map { String(rest[..<$0]) }
}

func color(_ hex: String) -> CGColor {
    let v = UInt32(hex.dropFirst(), radix: 16) ?? 0
    return CGColor(red: CGFloat((v >> 16) & 0xFF) / 255, green: CGFloat((v >> 8) & 0xFF) / 255, blue: CGFloat(v & 0xFF) / 255, alpha: 1)
}

let ctx = CGContext(data: nil, width: side, height: side, bitsPerComponent: 8, bytesPerRow: 0,
                    space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
ctx.setFillColor(color("#0A0512"))
ctx.fill(CGRect(x: 0, y: 0, width: side, height: side))
var rest = svg[...]
while let start = rest.range(of: "<rect"), let end = rest[start.upperBound...].range(of: "/>") {
    let tag = rest[start.upperBound..<end.lowerBound]
    rest = rest[end.upperBound...]
    let n = { (k: String) in Int(attribute(k, tag) ?? "") ?? 0 }
    guard let fill = attribute("fill", tag) else { continue }
    ctx.setFillColor(color(fill))
    // The SVG's y grows down, the context's up.
    ctx.fill(CGRect(x: margin + n("x") * unit, y: side - margin - (n("y") + n("height")) * unit, width: n("width") * unit,
                    height: n("height") * unit))
}
let out = CGImageDestinationCreateWithURL(URL(fileURLWithPath: args[2]) as CFURL, UTType.png.identifier as CFString, 1, nil)!
CGImageDestinationAddImage(out, ctx.makeImage()!, nil)
precondition(CGImageDestinationFinalize(out))
