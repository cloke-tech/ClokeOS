// _ObjectiveCBridgeable-Konformanzen für die Typen aus FoundationEssentials (dort fehlt die ObjC-Schicht)
import ObjectiveC

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
