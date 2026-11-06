// FileManager.enumerator(at:…): die ObjC-Methode mit Swift-URL und Swift-Fehlerbehandlung. Diese Datei importiert aus FoundationEssentials nur URL, damit FileManager
// eindeutig die Klasse NSFileManager des SDK meint (Essentials hat ein gleichnamiges FileManager).
import struct FoundationEssentials.URL
import ObjectiveC
import _SwiftFoundationOverlayShims

extension FileManager {
    public func enumerator(at url: URL, includingPropertiesForKeys keys: [URLResourceKey]? = nil, options mask: FileManager.DirectoryEnumerationOptions = [],
                           errorHandler handler: ((URL, any Error) -> Bool)? = nil) -> FileManager.DirectoryEnumerator? {
        let wrapped: ((NSURL?, (any Error)?) -> Bool)? = handler.map { h in { (u: NSURL?, e: (any Error)?) -> Bool in
            h(URL._unconditionallyBridgeFromObjectiveC(u), e ?? NSError(domain: NSCocoaErrorDomain, code: 256, userInfo: nil)) } }
        return clk_fm_enumerator(self, url._bridgeToObjectiveC(), keys, UInt(mask.rawValue), wrapped)
    }
}
