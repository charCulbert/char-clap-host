// Draws resources/clap-host.icns. The .icns is committed, so building the host
// needs no Swift; run this only to change the artwork:
//
//     swift resources/make-icon.swift
//
import AppKit

let root = URL(fileURLWithPath: CommandLine.arguments.first ?? ".")
    .deletingLastPathComponent()
let iconset = root.appendingPathComponent("clap-host.iconset")
try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)

func draw(_ size: CGFloat) -> NSBitmapImageRep {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size),
                               pixelsHigh: Int(size), bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                               colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)

    // macOS leaves a margin around the squircle; matching it keeps the icon the
    // same visual weight as everything else in the Dock.
    let inset = size * 0.055
    let box = NSRect(x: inset, y: inset, width: size - inset * 2, height: size - inset * 2)
    let plate = NSBezierPath(roundedRect: box, xRadius: box.width * 0.225,
                             yRadius: box.width * 0.225)
    NSGradient(starting: NSColor(srgbRed: 0.16, green: 0.18, blue: 0.24, alpha: 1),
               ending: NSColor(srgbRed: 0.07, green: 0.08, blue: 0.11, alpha: 1))!
        .draw(in: plate, angle: -90)

    let label = "CLAP" as NSString
    let font = NSFont.systemFont(ofSize: box.width * 0.235, weight: .heavy)
    let attrs: [NSAttributedString.Key: Any] = [
        .font: font,
        .foregroundColor: NSColor(srgbRed: 0.98, green: 0.99, blue: 1.0, alpha: 1),
        .kern: box.width * 0.016,
    ]
    let drawn = label.size(withAttributes: attrs)
    label.draw(at: NSPoint(x: box.midX - drawn.width / 2, y: box.midY - drawn.height / 2),
               withAttributes: attrs)

    NSGraphicsContext.restoreGraphicsState()
    return rep
}

for size in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let rep = draw(CGFloat(size * scale))
        let name = scale == 1 ? "icon_\(size)x\(size).png" : "icon_\(size)x\(size)@2x.png"
        try rep.representation(using: .png, properties: [:])!
            .write(to: iconset.appendingPathComponent(name))
    }
}

let convert = Process()
convert.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
convert.arguments = ["-c", "icns", iconset.path, "-o",
                     root.appendingPathComponent("clap-host.icns").path]
try convert.run()
convert.waitUntilExit()
try? FileManager.default.removeItem(at: iconset)
print("wrote \(root.appendingPathComponent("clap-host.icns").path)")
