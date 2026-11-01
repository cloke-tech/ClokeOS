// Minimales os-Overlay (ClokeOS): nur das, was FoundationEssentials und Logic aus dem Swift-Modul "os" brauchen.
// Apples Fassung ist nicht offen. Logger ist ein Wrapper um OS_os_log (hier als OSLog angesprochen); die Logger-Methoden (log, error, …)
// übersetzt der Aufrufer selbst (inlinable, Aufruf von _os_log_impl in libSystem).
import os

@_silgen_name("os_log_create") private func _clk_os_log_create(_ subsystem: UnsafePointer<CChar>, _ category: UnsafePointer<CChar>) -> OSLog

public struct Logger {
  public var logObject: OSLog { return _log }
  private let _log: OSLog
  public init(subsystem: String, category: String) { _log = _clk_os_log_create(subsystem, category) }
  public init(_ logObject: OSLog) { _log = logObject }
  public init() { _log = _clk_os_log_create("com.apple.default", "") }
}

extension OSLogType {
  public static var error: OSLogType { return OSLogType(rawValue: 0x10) }
  public static var fault: OSLogType { return OSLogType(rawValue: 0x11) }
}

extension OSLogType {
  public static var `default`: OSLogType { return OSLogType(rawValue: 0x00) }
  public static var info: OSLogType { return OSLogType(rawValue: 0x01) }
  public static var debug: OSLogType { return OSLogType(rawValue: 0x02) }
}

extension OSSignpostType {
  public static var event: OSSignpostType { return OSSignpostType(rawValue: 0x00) }
  public static var begin: OSSignpostType { return OSSignpostType(rawValue: 0x80) }
  public static var end: OSSignpostType { return OSSignpostType(rawValue: 0x81) }
}

public struct OSSignpostID: Equatable {
  public let rawValue: UInt64
  public init(_ value: UInt64) { rawValue = value }
  public init(log: OSLog) { rawValue = 0x1 }
  public static var exclusive: OSSignpostID { return OSSignpostID(0xEEEE_B0B5_B2B2_EEEE) }
}

public final class OSSignpostIntervalState {
  public let signpostID: OSSignpostID
  let isOpen: Bool
  public init(id: OSSignpostID, isOpen: Bool) { signpostID = id; self.isOpen = isOpen }
}

public enum OSSignpostError: Error { case doubleEnd }

public func checkForErrorAndConsumeState(state: OSSignpostIntervalState) -> OSSignpostError { return .doubleEnd }

extension OSLog {
  public var signpostsEnabled: Bool { return false }
  public static var disabled: OSLog { return _clk_os_log_create("com.apple.default", "disabled") }
}

public struct OSSignposter {
  public let logHandle: OSLog
  public init(logger: Logger) { logHandle = logger.logObject }
  public init(subsystem: String, category: String) { logHandle = _clk_os_log_create(subsystem, category) }
  public static var disabled: OSSignposter { return OSSignposter(subsystem: "com.apple.default", category: "disabled") }
}

// Alte Form: os_log(.info, dso:, log:, "…", args…): die Ausgabe fällt hier weg
public func os_log(_ type: OSLogType, dso: UnsafeRawPointer, log: OSLog, _ message: StaticString, _ args: CVarArg...) {}
