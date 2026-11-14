// SVG -> PNG für Fundus (auf dem Mac beim Bauen; Darlings AppKit lädt kein SVG).
//   swift render.swift <svg-ordner> <png-ordner>
// Ordner/Datei-Icons in 128 und 32 Pixel (name.png, name-32.png), Seitenleisten-Symbole in 32 (side-*.png).
import AppKit

let args = CommandLine.arguments
// render.swift --apps <stage>: AppIcon.svg jedes Programms (.app) zusätzlich als AppIcon.png (128 px), für FundusKit
if args.count > 2 && args[1] == "--apps" {
    var n = 0
    let fm = FileManager.default
    if let e = fm.enumerator(atPath: args[2]) {
        for case let p as String in e where p.hasSuffix(".app/Contents/Resources/AppIcon.svg") {
            let svg = URL(fileURLWithPath: args[2]).appendingPathComponent(p)
            let png = svg.deletingPathExtension().appendingPathExtension("png")
            if fm.fileExists(atPath: png.path) { continue }
            guard let img = NSImage(contentsOf: svg) else { continue }
            let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 128, pixelsHigh: 128, bitsPerSample: 8, samplesPerPixel: 4,
                                       hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
            img.draw(in: NSRect(x: 0, y: 0, width: 128, height: 128))
            NSGraphicsContext.restoreGraphicsState()
            try? rep.representation(using: .png, properties: [:])!.write(to: png)
            n += 1
        }
    }
    print("\(n) Programm-Icons als PNG")
    exit(0)
}
let src = URL(fileURLWithPath: args[1]), dst = URL(fileURLWithPath: args[2])
try? FileManager.default.createDirectory(at: dst, withIntermediateDirectories: true)

func render(_ img: NSImage, _ px: Int, _ to: URL) {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    NSGraphicsContext.current?.imageInterpolation = .high
    img.draw(in: NSRect(x: 0, y: 0, width: px, height: px))
    NSGraphicsContext.restoreGraphicsState()
    try! rep.representation(using: .png, properties: [:])!.write(to: to)
}

var n = 0
for f in try FileManager.default.contentsOfDirectory(at: src, includingPropertiesForKeys: nil) where f.pathExtension == "svg" {
    guard let img = NSImage(contentsOf: f) else { print("nicht lesbar: \(f.lastPathComponent)"); continue }
    let base = f.deletingPathExtension().lastPathComponent
    if base.hasPrefix("side-") {
        render(img, 32, dst.appendingPathComponent(base + ".png"))
    } else {
        render(img, 128, dst.appendingPathComponent(base + ".png"))
        render(img, 32, dst.appendingPathComponent(base + "-32.png"))
    }
    n += 1
}
print("\(n) PNG-Icons")
