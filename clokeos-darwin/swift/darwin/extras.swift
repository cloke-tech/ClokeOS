// Was das Darwin-Overlay von Apple zusätzlich bietet und die offenen Platform-Quellen nicht (ClokeOS)
public var MAP_FAILED: UnsafeMutableRawPointer! { return UnsafeMutableRawPointer(bitPattern: -1) }
public var noErr: Int32 { return 0 }
extension POSIXErrorCode {
  public static var EWOULDBLOCK: POSIXErrorCode { return .EAGAIN }
}
