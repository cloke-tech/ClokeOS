// Die Teile der Foundation-Überlagerung, die Apples geschlossene ObjC-Schicht liefert (Modul-ABI-Name Foundation).
import ObjectiveC

extension String {
    @inline(never)
    public func _bridgeToObjectiveC() -> NSString {
        var s = self
        return s.withUTF8 { clk_nsstring_utf8($0.baseAddress, $0.count) }
    }
}
