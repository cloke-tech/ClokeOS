// _ObjectiveCBridgeable-Konformanzen für die Typen aus FoundationEssentials (dort fehlt die ObjC-Schicht)
import ObjectiveC
import _SwiftFoundationOverlayShims

extension URL: _ObjectiveCBridgeable {
    public func _bridgeToObjectiveC() -> NSURL {
        if let base = baseURL { return NSURL(string: relativeString, relativeTo: base._bridgeToObjectiveC() as NSURL?)! }
        return NSURL(string: absoluteString) ?? NSURL(string: "")!
    }
    public static func _forceBridgeFromObjectiveC(_ source: NSURL, result: inout URL?) {
        result = URL._unconditionallyBridgeFromObjectiveC(source)
    }
    public static func _conditionallyBridgeFromObjectiveC(_ source: NSURL, result: inout URL?) -> Bool {
        result = URL._unconditionallyBridgeFromObjectiveC(source); return result != nil
    }
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSURL?) -> URL {
        guard let source = source else { return URL(fileURLWithPath: "") }
        if let base = source.baseURL { return URL(string: source.relativeString, relativeTo: URL._unconditionallyBridgeFromObjectiveC(base)) ?? URL(fileURLWithPath: "") }
        return URL(string: source.absoluteString ?? "") ?? URL(fileURLWithPath: "")
    }
}

extension Data: _ObjectiveCBridgeable {
    public func _bridgeToObjectiveC() -> NSData {
        return withUnsafeBytes { NSData(bytes: $0.baseAddress, length: $0.count) }
    }
    public static func _forceBridgeFromObjectiveC(_ source: NSData, result: inout Data?) {
        result = Data(bytes: source.bytes, count: source.length)
    }
    public static func _conditionallyBridgeFromObjectiveC(_ source: NSData, result: inout Data?) -> Bool {
        result = Data(bytes: source.bytes, count: source.length); return true
    }
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSData?) -> Data {
        guard let source = source else { return Data() }
        return Data(bytes: source.bytes, count: source.length)
    }
}

extension Date: _ObjectiveCBridgeable {
    public func _bridgeToObjectiveC() -> NSDate { return NSDate(timeIntervalSinceReferenceDate: timeIntervalSinceReferenceDate) }
    public static func _forceBridgeFromObjectiveC(_ source: NSDate, result: inout Date?) {
        result = Date(timeIntervalSinceReferenceDate: source.timeIntervalSinceReferenceDate)
    }
    public static func _conditionallyBridgeFromObjectiveC(_ source: NSDate, result: inout Date?) -> Bool {
        result = Date(timeIntervalSinceReferenceDate: source.timeIntervalSinceReferenceDate); return true
    }
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSDate?) -> Date {
        return Date(timeIntervalSinceReferenceDate: source?.timeIntervalSinceReferenceDate ?? 0)
    }
}

extension UUID: _ObjectiveCBridgeable {
    public func _bridgeToObjectiveC() -> NSUUID { return NSUUID(uuidString: uuidString)! }
    public static func _forceBridgeFromObjectiveC(_ source: NSUUID, result: inout UUID?) {
        result = UUID(uuidString: source.uuidString)
    }
    public static func _conditionallyBridgeFromObjectiveC(_ source: NSUUID, result: inout UUID?) -> Bool {
        result = UUID(uuidString: source.uuidString); return result != nil
    }
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSUUID?) -> UUID {
        return source.flatMap { UUID(uuidString: $0.uuidString) } ?? UUID()
    }
}

// ---- weitere Teile der ObjC-Schicht, die FoundationEssentials nicht hat ----

extension URL {
    public func checkResourceIsReachable() throws -> Bool { if access(path, F_OK) == 0 { return true }
        throw NSError(domain: NSCocoaErrorDomain, code: 260, userInfo: [NSFilePathErrorKey: path]) }
    public func startAccessingSecurityScopedResource() -> Bool { return _bridgeToObjectiveC().startAccessingSecurityScopedResource() }
    public func stopAccessingSecurityScopedResource() { _bridgeToObjectiveC().stopAccessingSecurityScopedResource() }
    public static var desktopDirectory: URL { return URL(fileURLWithPath: NSHomeDirectory() + "/Desktop", isDirectory: true) }
    // Schlüsseltyp wie in Apples Signatur (NSURLResourceKey ist eine Hülle um NSString, gleiche Darstellung; deshalb Set<NSString> und ein Alias)
    public func resourceValues(forKeys keys: Set<NSString>) throws -> URLResourceValues {
        var v = URLResourceValues()
        var err: NSError? = nil
        guard let d = clk_url_resource_values(_bridgeToObjectiveC(), Array(keys), &err) else { throw err ?? NSError(domain: NSCocoaErrorDomain, code: 256, userInfo: nil) }
        for (k, x) in d { v._values[(k as? String) ?? "\(k)"] = x }
        return v
    }
    public mutating func setResourceValues(_ values: URLResourceValues) throws {
        var err: NSError? = nil
        if !clk_url_set_resource_values(_bridgeToObjectiveC(), values._values, &err) { throw err ?? NSError(domain: NSCocoaErrorDomain, code: 256, userInfo: nil) }
    }
    public func bookmarkData(options: NSURL.BookmarkCreationOptions = [], includingResourceValuesForKeys keys: Set<NSString>? = nil, relativeTo url: URL? = nil) throws -> Data {
        var err: NSError? = nil
        guard let d = clk_url_bookmark(_bridgeToObjectiveC(), UInt(options.rawValue), keys.map { Array($0) }, url?._bridgeToObjectiveC(), &err) else { throw err ?? NSError(domain: NSCocoaErrorDomain, code: 256, userInfo: nil) }
        return Data._unconditionallyBridgeFromObjectiveC(d)
    }
    public init(resolvingBookmarkData data: Data, options: NSURL.BookmarkResolutionOptions = [], relativeTo url: URL? = nil, bookmarkDataIsStale: inout Bool) throws {
        var stale: ObjCBool = false
        let n = try NSURL(resolvingBookmarkData: data._bridgeToObjectiveC(), options: options, relativeTo: url?._bridgeToObjectiveC(), bookmarkDataIsStale: &stale)
        bookmarkDataIsStale = stale.boolValue
        self = URL._unconditionallyBridgeFromObjectiveC(n)
    }
}

public struct URLResourceValues {
    fileprivate var _values: [String: Any] = [:]
    public init() {}
    public var isReadable: Bool? { return _values["NSURLIsReadableKey"] as? Bool }
    public var isDirectory: Bool? { return _values["NSURLIsDirectoryKey"] as? Bool }
    public var isRegularFile: Bool? { return _values["NSURLIsRegularFileKey"] as? Bool }
    public var creationDate: Date? { return _values["NSURLCreationDateKey"] as? Date }
    public var fileSize: Int? { return _values["NSURLFileSizeKey"] as? Int }
    public var isExcludedFromBackup: Bool? {
        get { return _values["NSURLIsExcludedFromBackupKey"] as? Bool }
        set { _values["NSURLIsExcludedFromBackupKey"] = newValue }
    }
}

extension Data {
    public init(contentsOf url: URL, options: NSData.ReadingOptions = []) throws {
        self = Data._unconditionallyBridgeFromObjectiveC(try NSData(contentsOf: url._bridgeToObjectiveC() as NSURL, options: options))
    }
    public func write(to url: URL, options: NSData.WritingOptions = []) throws {
        try _bridgeToObjectiveC().write(to: url._bridgeToObjectiveC() as NSURL, options: options)
    }
}

extension Locale: _ObjectiveCBridgeable {
    public func _bridgeToObjectiveC() -> NSLocale { return NSLocale(localeIdentifier: identifier) }
    public static func _forceBridgeFromObjectiveC(_ source: NSLocale, result: inout Locale?) { result = Locale(identifier: source.localeIdentifier) }
    public static func _conditionallyBridgeFromObjectiveC(_ source: NSLocale, result: inout Locale?) -> Bool {
        result = Locale(identifier: source.localeIdentifier); return true
    }
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSLocale?) -> Locale {
        return Locale(identifier: source?.localeIdentifier ?? "en_US")
    }
}

private func _ns<S: StringProtocol>(_ s: S) -> NSString { return NSString(string: String(s)) }
private func _nsRange<S: StringProtocol>(_ s: S, _ r: Range<String.Index>?) -> NSRange {
    let str = String(s)
    return r.map { NSRange($0, in: str) } ?? NSRange(location: 0, length: (str as NSString).length)
}
private func _range<S: StringProtocol>(_ s: S, _ r: NSRange) -> Range<String.Index>? {
    return r.location == NSNotFound ? nil : Range(r, in: String(s))
}

extension StringProtocol {
    public func components(separatedBy separator: CharacterSet) -> [String] { return _ns(self).components(separatedBy: separator) }
    public func trimmingCharacters(in set: CharacterSet) -> String { return _ns(self).trimmingCharacters(in: set) }
    public func addingPercentEncoding(withAllowedCharacters set: CharacterSet) -> String? { return _ns(self).addingPercentEncoding(withAllowedCharacters: set) }
    public var removingPercentEncoding: String? { return _ns(self).removingPercentEncoding }
    public var localizedUppercase: String { return _ns(self).localizedUppercase }
    public func rangeOfCharacter(from set: CharacterSet, options: NSString.CompareOptions = [], range: Range<String.Index>? = nil) -> Range<String.Index>? {
        return _range(self, _ns(self).rangeOfCharacter(from: set, options: options, range: _nsRange(self, range)))
    }
    public func replacingOccurrences<Target: StringProtocol, Replacement: StringProtocol>(of target: Target, with replacement: Replacement, options: NSString.CompareOptions = [], range: Range<String.Index>? = nil) -> String {
        return _ns(self).replacingOccurrences(of: String(target), with: String(replacement), options: options, range: _nsRange(self, range))
    }
    public func range<T: StringProtocol>(of aString: T, options: NSString.CompareOptions = [], range: Range<String.Index>? = nil, locale: Locale? = nil) -> Range<String.Index>? {
        return _range(self, _ns(self).range(of: String(aString), options: options, range: _nsRange(self, range), locale: locale?._bridgeToObjectiveC()))
    }
    public func compare<T: StringProtocol>(_ aString: T, options: NSString.CompareOptions = [], range: Range<String.Index>? = nil, locale: Locale? = nil) -> ClkComparisonResult {
        return _ns(self).compare(String(aString), options: options, range: _nsRange(self, range), locale: locale?._bridgeToObjectiveC())
    }
    public func padding<T: StringProtocol>(toLength newLength: Int, withPad padString: T, startingAt padIndex: Int) -> String {
        return _ns(self).padding(toLength: newLength, withPad: String(padString), startingAt: padIndex)
    }
    public func contains<T: StringProtocol>(_ other: T) -> Bool { return _ns(self).range(of: String(other)).location != NSNotFound }
}

extension String {
    public init?(utf8String bytes: UnsafePointer<CChar>) { self.init(validatingCString: bytes) }
    public init(format: __shared String, _ arguments: CVarArg...) { self = NSString(format: format, arguments: getVaList(arguments)) as String }
    public init(format: __shared String, arguments: __shared [CVarArg]) { self = NSString(format: format, arguments: getVaList(arguments)) as String }
    public init(format: __shared String, locale: __shared Locale?, _ args: CVarArg...) {
        self = NSString(format: format, locale: locale?._bridgeToObjectiveC(), arguments: getVaList(args)) as String
    }
    public init(format: __shared String, locale: __shared Locale?, arguments: __shared [CVarArg]) {
        self = NSString(format: format, locale: locale?._bridgeToObjectiveC(), arguments: getVaList(arguments)) as String
    }
    public static func localizedStringWithFormat(_ format: String, _ arguments: CVarArg...) -> String {
        return String(format: format, locale: Locale.current, arguments: arguments)
    }
}
