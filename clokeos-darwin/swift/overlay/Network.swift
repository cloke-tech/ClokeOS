// Minimales Network-Overlay (ClokeOS): NWPathMonitor/NWPath, so viel wie Logic Pro importiert. Es gibt keinen Netzwerkpfad-Dienst, der Monitor meldet "unsatisfied".
import Dispatch

public struct NWPath {
  public enum Status: Equatable, Sendable { case satisfied, unsatisfied, requiresConnection }
  public var status: Status
  public init(status: Status = .unsatisfied) { self.status = status }
}

public final class NWPathMonitor {
  public var pathUpdateHandler: ((NWPath) -> Void)?
  public var currentPath: NWPath { return NWPath() }
  public init() {}
  public func start(queue: DispatchQueue) {
    let handler = pathUpdateHandler
    queue.async { handler?(NWPath()) }
  }
  public func cancel() { pathUpdateHandler = nil }
}
