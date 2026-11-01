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
