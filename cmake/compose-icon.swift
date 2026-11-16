// Turns a flat artwork PNG (docs/app-icon.png) into a 1024x1024 macOS app
// icon: the artwork centred on a light squircle with the standard macOS
// padding. Icons that are not shaped like this get a gray plate in macOS 26.
// Usage: swift compose-icon.swift <artwork.png> <out.png>
import AppKit

let args = CommandLine.arguments
guard let artwork = NSImage(contentsOfFile: args[1]) else { fatalError("cannot read \(args[1])") }

let canvas: CGFloat = 1024
let plate = NSRect(x: 100, y: 100, width: 824, height: 824)   // macOS icon grid
let radius: CGFloat = 185

let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(canvas), pixelsHigh: Int(canvas),
                           bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                           colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
NSGraphicsContext.current?.imageInterpolation = .high

// Squircle plate with a subtle top-to-bottom gradient.
let shape = NSBezierPath(roundedRect: plate, xRadius: radius, yRadius: radius)
NSGradient(starting: NSColor(calibratedWhite: 1.0, alpha: 1), ending: NSColor(calibratedWhite: 0.90, alpha: 1))!
    .draw(in: shape, angle: -90)

// Artwork at ~62% of the plate, centred.
let side = plate.width * 0.62
artwork.draw(in: NSRect(x: (canvas - side) / 2, y: (canvas - side) / 2, width: side, height: side),
             from: .zero, operation: .sourceOver, fraction: 1)

NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: args[2]))
