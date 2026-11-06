// Handgeschriebene SwiftUI-Funktionen (ClokeOS), zusammen mit den Platzhaltern aus swift/swiftui-gen.py als Modul SwiftUI übersetzt.
// Werte tragen vorerst nur ihre Daten; gezeichnet wird noch nichts.
import ObjectiveC
import Foundation

/// Bildquelle eines Image: Name und Bundle (für das spätere Laden über NSImage)
final class ClkNamedImageProvider: AnyImageProviderBox {
    let name: String
    let bundle: Bundle?
    init(name: String, bundle: Bundle?) { self.name = name; self.bundle = bundle; super.init() }
}

extension Image {
    public init(_ name: String, bundle: Bundle? = nil) {
        self.init(provider: ClkNamedImageProvider(name: name, bundle: bundle))
    }
}

// NSHostingView: AppKit-Sicht mit einer SwiftUI-Sicht als Inhalt. Vorerst nur die Hülle (zeichnet nichts).
import AppKit

open class NSHostingView<Content: View>: NSView {
    public var rootView: Content
    public required init(rootView: Content) {
        self.rootView = rootView
        super.init(frame: NSRect(x: 0, y: 0, width: 0, height: 0))
    }
    public required init?(coder: NSCoder) { return nil }
}
