// _ObjectiveCBridgeable-Konformanzen für die Typen aus FoundationEssentials (dort fehlt die ObjC-Schicht)
import ObjectiveC
import _Concurrency
import Darwin
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
    public init(resolvingBookmarkData data: __shared Data, options: NSURL.BookmarkResolutionOptions = [], relativeTo url: __shared URL? = nil, bookmarkDataIsStale: inout Bool) throws {
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
    // __shared wie in Apples Signatur: Initialisatoren nehmen Parameter sonst als "owned" (+1), Logic übergibt aber +0 und der überzählige Release gäbe die URL des Aufrufers frei
    public init(contentsOf url: __shared URL, options: NSData.ReadingOptions = []) throws {
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

// Bundle(__dsoHandle:): das Bundle, in dem der Code mit diesem Handle liegt (#dsohandle in Swift-Code, der Bundle.module / Bundle(for:) braucht)
extension Bundle {
    public convenience init?(__dsoHandle handle: UnsafeRawPointer) {
        var info = Dl_info()
        guard dladdr(handle, &info) != 0, let name = info.dli_fname else { return nil }
        var url = URL(fileURLWithPath: String(cString: name))
        while url.path != "/" && !url.path.isEmpty {
            url.deleteLastPathComponent()
            let ext = url.pathExtension
            if ext == "framework" || ext == "app" || ext == "bundle" || ext == "xpc" || ext == "appex" { self.init(path: url.path); return }
        }
        return nil
    }
}

// Locale.Language.region steckt in FoundationInternationalization (ICU, nicht gebaut); die Komponenten lassen sich über Reflection lesen
extension Locale.Language {
    public var region: Locale.Region? {
        for child in Mirror(reflecting: self).children where child.label == "components" {
            if let c = child.value as? Locale.Language.Components { return c.region }
        }
        return nil
    }
}

// URLRequest (FoundationNetworking bei Apple im Foundation-Modul): so viel wie Logic Pro braucht, mit Brücke zu NSURLRequest
public struct URLRequest: Equatable, Hashable, CustomStringConvertible, _ObjectiveCBridgeable {
    public var url: URL?
    public var cachePolicy: NSURLRequest.CachePolicy
    public var timeoutInterval: TimeInterval
    public var httpMethod: String? = "GET"
    public var allHTTPHeaderFields: [String: String]?
    public var httpBody: Data?
    public var mainDocumentURL: URL?
    public init(url: URL, cachePolicy: NSURLRequest.CachePolicy = .useProtocolCachePolicy, timeoutInterval: TimeInterval = 60.0) {
        self.url = url; self.cachePolicy = cachePolicy; self.timeoutInterval = timeoutInterval
    }
    public var description: String { return url?.absoluteString ?? "(null)" }
    public mutating func setValue(_ value: String?, forHTTPHeaderField field: String) {
        var h = allHTTPHeaderFields ?? [:]; h[field] = value; allHTTPHeaderFields = h
    }
    public func value(forHTTPHeaderField field: String) -> String? { return allHTTPHeaderFields?[field] }

    public func _bridgeToObjectiveC() -> NSURLRequest {
        let r = NSMutableURLRequest(url: (url ?? URL(fileURLWithPath: ""))._bridgeToObjectiveC(),
                                    cachePolicy: cachePolicy, timeoutInterval: timeoutInterval)
        r.httpMethod = httpMethod ?? "GET"
        if let h = allHTTPHeaderFields { r.allHTTPHeaderFields = h }
        if let b = httpBody { r.httpBody = b._bridgeToObjectiveC() }
        return r
    }
    public static func _forceBridgeFromObjectiveC(_ source: NSURLRequest, result: inout URLRequest?) {
        result = URLRequest._unconditionallyBridgeFromObjectiveC(source)
    }
    public static func _conditionallyBridgeFromObjectiveC(_ source: NSURLRequest, result: inout URLRequest?) -> Bool {
        result = URLRequest._unconditionallyBridgeFromObjectiveC(source); return true
    }
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSURLRequest?) -> URLRequest {
        var r = URLRequest(url: source?.url.map { URL._unconditionallyBridgeFromObjectiveC($0) } ?? URL(fileURLWithPath: ""),
                           cachePolicy: source?.cachePolicy ?? .useProtocolCachePolicy, timeoutInterval: source?.timeoutInterval ?? 60.0)
        r.httpMethod = source?.httpMethod
        r.allHTTPHeaderFields = source?.allHTTPHeaderFields
        return r
    }
}

extension URLQueryItem: _ObjectiveCBridgeable {
    public func _bridgeToObjectiveC() -> NSURLQueryItem { return NSURLQueryItem(name: name, value: value) }
    public static func _forceBridgeFromObjectiveC(_ source: NSURLQueryItem, result: inout URLQueryItem?) { result = URLQueryItem(name: source.name, value: source.value) }
    public static func _conditionallyBridgeFromObjectiveC(_ source: NSURLQueryItem, result: inout URLQueryItem?) -> Bool {
        result = URLQueryItem(name: source.name, value: source.value); return true
    }
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSURLQueryItem?) -> URLQueryItem {
        return URLQueryItem(name: source?.name ?? "", value: source?.value)
    }
}

// URLSession.data(from:delegate:): es gibt kein Netz in der VM; die Anfrage scheitert wie auf einem Rechner ohne Verbindung (NSURLErrorNotConnectedToInternet)
extension URLSession {
    public func data(from url: URL, delegate: (any URLSessionTaskDelegate)? = nil) async throws -> (Data, URLResponse) {
        throw NSError(domain: NSURLErrorDomain, code: -1009, userInfo: [NSLocalizedDescriptionKey: "Die Internetverbindung scheint offline zu sein."])
    }
}
