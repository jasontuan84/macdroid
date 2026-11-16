// Draws the DMG window background: an arrow from the app icon to the
// Applications folder and a short hint.
// Usage: swift dmg-background.swift <out.png> <width> <height> <scale> <iconCentreY>
import AppKit

let args = CommandLine.arguments
let out = args[1]
let width = CGFloat(Double(args[2])!), height = CGFloat(Double(args[3])!)
let scale = CGFloat(Double(args[4])!)
let iconY = CGFloat(Double(args[5])!)   // icon centre, from the top of the window

let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(width * scale), pixelsHigh: Int(height * scale),
                           bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                           colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
rep.size = NSSize(width: width, height: height)
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)

// Soft light gradient (Finder shows DMG windows in light text by default).
NSGradient(starting: NSColor(calibratedWhite: 0.98, alpha: 1), ending: NSColor(calibratedWhite: 0.92, alpha: 1))!
    .draw(in: NSRect(x: 0, y: 0, width: width, height: height), angle: -90)

// Arrow between the two icon positions, level with the icon centres.
let y = height - iconY
let arrow = NSBezierPath()
arrow.move(to: NSPoint(x: 250, y: y))
arrow.line(to: NSPoint(x: 400, y: y))
arrow.lineWidth = 6
arrow.lineCapStyle = .round
NSColor(calibratedWhite: 0.55, alpha: 1).setStroke()
arrow.stroke()
let head = NSBezierPath()
head.move(to: NSPoint(x: 412, y: y))
head.line(to: NSPoint(x: 392, y: y + 16))
head.line(to: NSPoint(x: 392, y: y - 16))
head.close()
NSColor(calibratedWhite: 0.55, alpha: 1).setFill()
head.fill()

// Hint text.
let style = NSMutableParagraphStyle()
style.alignment = .center
let attrs: [NSAttributedString.Key: Any] = [
    .font: NSFont.systemFont(ofSize: 15, weight: .medium),
    .foregroundColor: NSColor(calibratedWhite: 0.35, alpha: 1),
    .paragraphStyle: style,
]
NSAttributedString(string: "Drag MacDroid to Applications to install", attributes: attrs)
    .draw(in: NSRect(x: 0, y: 64, width: width, height: 24))   // near the bottom (Finder hides ~28 pt under the title bar)

NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
