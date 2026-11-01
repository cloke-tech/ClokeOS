#!/bin/bash
# Baut <Swift-Ordner>/run-arm64/fo2.dylib: die ObjC-Schicht des Foundation-Overlays (NSArray/NSDictionary/NSSet/NSString, IndexSet, CharacterSet,
# Notification, NSError, NSNumber, KVO …) aus Apples altem, offenem Overlay (Swift 5.3, stdlib/public/Darwin/Foundation; ABI-stabil),
# dazu die Overlays ObjectiveC, CoreGraphics (CGFloat), Dispatch und Darwin (MachError) aus demselben Stand. Alles mit dem ABI-Namen des Moduls,
# gegen die Foundation-Header des SDK, und auf fo.dylib (swift/foundation-overlay.sh) gelinkt. Quellen: raw.githubusercontent.com/apple/swift (rund 2 MB).
#
#   swift/foundation-objc.sh [Swift-Ordner (swiftsrc)] [Ordner mit den FoundationEssentials-Objekten]   Standard: $CLOKEOS_WORK/swiftsrc
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
sw="${1:-${CLOKEOS_WORK:-$HOME/ClokeOS-work}/swiftsrc}"
fb="${2:-${CLOKEOS_WORK:-$HOME/ClokeOS-work}/swift-foundation-build/swift-foundation/.build/release}"
fs="${fb%/.build/*}"   # swift-foundation-Ordner (Sources/_FoundationCShims)
run="$sw/run-arm64"; w="$sw/fnov"; tag=swift-5.3-RELEASE
raw="https://raw.githubusercontent.com/apple/swift/$tag/stdlib/public"
sdk="$(xcrun --show-sdk-path)"
mkdir -p "$w/src" "$w/shims" "$w/out"
get() { [ -s "$w/$2" ] || curl -fsSL "$raw/$1" -o "$w/$2"; }
# Foundation-Overlay: nur diese Dateien (Rest steckt in FoundationEssentials oder braucht Combine/URLSession)
FILES="Boxing CharacterSet IndexSet NSIndexSet Notification NSArray NSDictionary NSSet NSString NSError NSObject NSNumber NSRange NSFastEnumeration ReferenceConvertible String NSURL Foundation"
for f in $FILES; do get "Darwin/Foundation/$f.swift" "src/$f.swift"; done
for h in CoreFoundationOverlayShims CoreFoundationShims FoundationOverlayShims FoundationShimSupport FoundationShims CFCharacterSetShims CFHashingShims NSCalendarShims NSCharacterSetShims NSCoderShims NSDataShims NSDictionaryShims NSErrorShims NSFileManagerShims NSIndexPathShims NSIndexSetShims NSKeyedArchiverShims NSLocaleShims NSTimeZoneShims NSUndoManagerShims ObjectiveCOverlayShims DispatchOverlayShims; do get "SwiftShims/$h.h" "shims/$h.h"; done
grep -q ClkComparisonResult "$w/shims/FoundationOverlayShims.h" || printf '\n#import <Foundation/Foundation.h>\ntypedef NSComparisonResult ClkComparisonResult;\nNSDictionary * _Nullable clk_url_resource_values(NSURL *url, NSArray *keys, NSError **err);\nBOOL clk_url_set_resource_values(NSURL *url, NSDictionary *values, NSError **err);\nNSData * _Nullable clk_url_bookmark(NSURL *url, NSUInteger options, NSArray * _Nullable keys, NSURL * _Nullable relative, NSError **err);\n' >> "$w/shims/FoundationOverlayShims.h"   # eindeutige Namen für SDK-Typen, die FoundationEssentials gleichnamig hat
cp "$here/overlay/shims.modulemap" "$w/shims/module.modulemap"
get Darwin/Foundation/NSValue.swift.gyb src/NSValue.swift.gyb
get Darwin/ObjectiveC/ObjectiveC.swift src/ObjectiveC.swift
get Darwin/CoreGraphics/CGFloat.swift.gyb src/CGFloat.swift.gyb
get Platform/MachError.swift src/MachError.swift
for n in Block Data Dispatch IO Queue Source Time Private; do get "Darwin/Dispatch/$n.swift" "src/Disp$n.swift"; done
# Anpassungen für Swift 6.4 und das SDK 27 (Kommentare dort, wo sie nötig sind)
python3 -I - "$w/src" <<'PY'
import re, sys, os
d = sys.argv[1]
def sub(f, a, b, n=1):
    p = os.path.join(d, f); t = open(p).read()
    if a in t: open(p, "w").write(t.replace(a, b))
for f in ("Foundation.swift", "NSNumber.swift"):
    p = os.path.join(d, f)
    if os.path.exists(p):
        s = open(p).read().replace("import CoreGraphics", "import CoreFoundation")
        open(p, "w").write(s)
sub("NSNumber.swift", "AnyHashable(nsDecimalNumber.decimalValue)", "AnyHashable(nsDecimalNumber)")
for f, l in (("NSArray.swift", "extension Array: CVarArg {}"), ("NSDictionary.swift", "extension Dictionary: CVarArg {}"),
             ("NSSet.swift", "extension Set: CVarArg {}"), ("String.swift", "extension String: CVarArg {}")):
    sub(f, l + "\n", "")     # die Konformanz liefert das SDK-Stdlib-Interface schon
sub("String.swift", "self = String(_cocoaString: cocoaString)", 'self = cocoaString.utf8String.map { String(cString: $0) } ?? ""')
sub("CharacterSet.swift", "CFCharacterSetCreateBitmapRepresentation(nil, cs) as Data", "_clkData(unsafeBitCast(CFCharacterSetCreateBitmapRepresentation(nil, cs), to: NSData.self))")
sub("CharacterSet.swift", "data as CFData", "_clkCFData(data)")
p = os.path.join(d, "CharacterSet.swift"); t = open(p).read()
if "_clkData(_" not in t:
    open(p, "w").write(t + """
// Hilfen ohne Apples NSData-Brücke (Data kommt aus FoundationEssentials)
fileprivate func _clkData(_ d: NSData) -> Data { return Data(bytes: d.bytes, count: d.length) }
fileprivate func _clkCFData(_ d: Data) -> CFData {
    return d.withUnsafeBytes { unsafeBitCast(NSData(bytes: $0.baseAddress, length: $0.count), to: CFData.self) }
}
""")
sub("CharacterSet.swift", "options: .mappedIfSafe)", "options: NSData.ReadingOptions.mappedIfSafe)")
sub("IndexSet.swift", "__NSIndexSetIndexOfRangeContainingIndex($0, integer)", "__NSIndexSetIndexOfRangeContainingIndex($0, UInt(integer))")
sub("IndexSet.swift", "__NSIndexSetRangeAtIndex($0, index,", "__NSIndexSetRangeAtIndex($0, UInt(index),")
for a in ("__NSDictionaryGetObjects(d, objectVals, objectKeys, numElems)", "__NSDictionaryGetObjects(x, objectVals, objectKeys, numElems)",
          "__NSDictionaryGetObjects(otherDictionary, buffer, buffer2, numElems)"):
    sub("NSDictionary.swift", a, a[:-9] + "UInt(numElems))")
# der Compiler stürzt bei NSNotification.userInfo ab (IRGen): Umweg über die Laufzeit
sub("Notification.swift", "userInfo: x.userInfo)", "userInfo: _clkUserInfo(x))")
sub("Notification.swift", "userInfo: src.userInfo)", "userInfo: _clkUserInfo(src))")
p = os.path.join(d, "Notification.swift"); t = open(p).read()
if "_clkUserInfo(_" not in t:
    open(p, "w").write(t + """
fileprivate func _clkUserInfo(_ n: NSNotification) -> [AnyHashable: Any]? {
    return n.perform(Selector(("userInfo")))?.takeUnretainedValue() as? [AnyHashable: Any]
}
""")
PY
cp "$here/overlay/Bridging.swift" "$w/src/Bridging.swift"
printf '@_exported import FoundationEssentials\n' > "$w/src/Prelude.swift"
gyb="$sw/swift/utils/gyb.py"
python3 "$gyb" --line-directive '' -o "$w/src/NSValue.swift" "$w/src/NSValue.swift.gyb"
python3 "$gyb" -DCMAKE_SIZEOF_VOID_P=8 --line-directive '' -o "$w/src/CGFloat.swift" "$w/src/CGFloat.swift.gyb"
sed -i '' 's/^import CoreGraphics/import CoreFoundation/' "$w/src/NSValue.swift"
common=(-c -Onone -wmo -parse-as-library -swift-version 5 -enable-library-evolution -target arm64-apple-macos26.0 -sdk "$sdk" -Xcc "-I$w/shims" -I "$w/shims"
        -Xfrontend -disable-implicit-concurrency-module-import -Xfrontend -disable-implicit-string-processing-module-import)
fo=(); for f in Prelude $FILES Bridging; do fo+=("$w/src/$f.swift"); done
# NSValue/NSNumber brauchen CGFloat aus dem SDK; NSValue wird mit übersetzt, wenn es sich übersetzen lässt
swiftc "${common[@]}" -module-name Foundation -Xfrontend -import-underlying-module -I "$fb" \
  -Xcc "-I$fs/Sources/_FoundationCShims/include" "${fo[@]}" -o "$w/out/fnov.o"
ovc() { m=$1; shift; swiftc "${common[@]}" -module-name "$m" -Xfrontend -import-underlying-module "$@" -o "$w/out/ov$m.o"; }
ovc ObjectiveC "$w/src/ObjectiveC.swift"
ovc CoreGraphics "$w/src/CGFloat.swift"
ovc Dispatch "$w"/src/Disp*.swift
ovc Darwin "$w/src/MachError.swift"
# Logics Code importiert z. B. URL._bridgeToObjectiveC unter dem Namen der Typ-eigenen Methode; meine Fassung steht als Erweiterung ("10FoundationE")
# in einem anderen Modul und heißt deshalb anders. Aliase (ld -alias, gilt auch für Deskriptoren und Metadaten): dieselben Namen zeigen auf meine Fassung.
imports="${CLOKE_IMPORTS:-${CLOKEOS_WORK:-$HOME/ClokeOS-work}/out/stubgen/imports.json}"
if [ -f "$imports" ]; then
python3 -I - "$imports" "$w/out/fnov.o" "$w/out/alias.s" "$run/fo.dylib" <<'PY'
import json, subprocess, sys
imp, obj, out, fo = sys.argv[1:5]
def demangle(names):
    r = subprocess.run(["xcrun", "swift-demangle", "--simplified"][:2] if False else ["xcrun", "swift-demangle"], input="\n".join(names), capture_output=True, text=True).stdout.splitlines()
    return [l.replace("__shared ", "").replace("static ", "").split(" for ")[0] for l in r]
norm = lambda s: s.replace("(extension in Foundation):", "").replace("NSURLResourceKey", "NSString")   # NSURLResourceKey ist eine Hülle um NSString (gleiche Darstellung)
mine = [l.split()[-1] for l in subprocess.run(["nm", "-gU", obj], capture_output=True, text=True).stdout.splitlines()]
mine = [m for m in mine if m.startswith("_$s")]
dm = dict(zip((norm(d) for d in demangle(mine)), mine))
have = set(mine) | {l.split()[-1] for l in subprocess.run(["nm", "-gU", fo], capture_output=True, text=True).stdout.splitlines()}   # was fo.dylib schon liefert, nicht doppeln
want = [s for s in json.load(open(imp)).get("Foundation", {}).get("symbols", []) if s.startswith("_$s") and s not in have]
n = 0
with open(out, "w") as f:
    for s, d in zip(want, demangle(want)):
        m = dm.get(norm(d))
        if m and m != s and "(extension in Foundation)" not in d:
            f.write("%s %s\n" % (m, s)); n += 1
print("Aliase:", n, file=sys.stderr)
PY
else : > "$w/out/alias.s"; fi
aliasargs=(); while read -r a b; do aliasargs+=(-alias "$a" "$b"); done < "$w/out/alias.s"
clang -arch arm64 -c "$here/overlay/dstub.c" -o "$w/out/dstub.o"
clang -arch arm64 -fno-objc-arc -c -x objective-c -isysroot "$sdk" "$here/overlay/dstub.m" -o "$w/out/dstub2.o"
cat > "$w/out/sys.tbd" <<'T'
--- !tapi-tbd
tbd-version: 4
targets: [ arm64-macos ]
install-name: '/usr/lib/libSystemX.B.dylib'
current-version: 1292
compatibility-version: 1
exports:
  - targets: [ arm64-macos ]
    symbols: [ dummy_sym ]
...
T
ld -arch arm64 -dylib -o "$run/fo2.dylib" "$w"/out/fnov.o "$w"/out/ovObjectiveC.o "$w"/out/ovCoreGraphics.o "$w"/out/ovDispatch.o "$w"/out/ovDarwin.o \
  "$w"/out/dstub.o "$w"/out/dstub2.o "${aliasargs[@]}" "$run/fo.dylib" "$run/libswiftCore.dylib" "$run/libswift_Concurrency.dylib" "$run/libswiftDarwin.dylib" "$w/out/sys.tbd" \
  -platform_version macos 11.0 11.0 -install_name /usr/lib/sov/fo2.dylib -rpath /usr/lib/swift -not_for_dyld_shared_cache -undefined dynamic_lookup \
  -L"$sdk/usr/lib" -lobjc 2>&1 | grep -v warning || true
install_name_tool -change /usr/lib/libSystemX.B.dylib /usr/lib/libSystem.B.dylib "$run/fo2.dylib"
echo "$run/fo2.dylib"
