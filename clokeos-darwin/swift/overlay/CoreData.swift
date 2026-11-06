// Minimales CoreData-Swift-Overlay (ClokeOS): die vier Swift-Symbole, die Logic Pro importiert. Hinter den ObjC-Klassen steht kein echter Speicher (Stub-Framework),
// deshalb liefern fetch/count leere Ergebnisse; performAndWait führt den Block direkt aus.
import Combine
import ObjectiveC

private var _clkPublisherKey: UInt8 = 0

extension NSManagedObjectContext {
    public func performAndWait<T>(_ block: () throws -> T) throws -> T { return try block() }
    public func count<T: NSFetchRequestResult>(for request: NSFetchRequest<T>) throws -> Int { return 0 }
    public func fetch<T: NSFetchRequestResult>(_ request: NSFetchRequest<T>) throws -> [T] { return [] }
}

extension NSManagedObject: ObservableObject {
    public var objectWillChange: ObservableObjectPublisher {
        if let p = objc_getAssociatedObject(self, &_clkPublisherKey) as? ClkPublisherBox { return p.publisher }
        let box = ClkPublisherBox()
        objc_setAssociatedObject(self, &_clkPublisherKey, box, .OBJC_ASSOCIATION_RETAIN)
        return box.publisher
    }
}

private final class ClkPublisherBox { let publisher = ObservableObjectPublisher() }
