#!/bin/bash
# Linkt das Foundation-Swift-Overlay für arm64 zu <Arbeitsordner>/run-arm64/fo.dylib (Installname /usr/lib/sov/fo.dylib):
#   FoundationEssentials (swift/foundation.sh) + die Collections-Objekte + _FoundationCShims
#   + os (swift/overlay/os.swift) und Combine (swift/overlay/Combine.swift: nur die Protokolle TopLevelEncoder/-Decoder)
# gegen die Swift-Laufzeit aus swift/build.sh, swift/supplemental.sh und swift/darwin.sh. macapps/stubgen/swiftov.py lässt das Foundation-
# Stub-Framework fo.dylib re-exportieren und nimmt die Symbole, die fo.dylib liefert, aus den Trap-Marken heraus.
#
#   swift/foundation-overlay.sh [Swift-Ordner] [swift-foundation-Ordner]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
sw="${1:-${CLOKEOS_WORK:-$HOME/ClokeOS-work}/swiftsrc}"
fb="${2:-${CLOKEOS_WORK:-$HOME/ClokeOS-work}/swift-foundation-build/swift-foundation/.build/release}"
run="$sw/run-arm64"
t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
flags=(-target arm64-apple-macos11.0 -parse-as-library -O -enable-library-evolution
       -Xfrontend -disable-implicit-concurrency-module-import -Xfrontend -disable-implicit-string-processing-module-import)
# os und Combine: Modulnamen wie bei Apple (ABI-Name), damit die Symbole $s2os… und $s7Combine… heißen
swiftc -c -module-name ClokeOsOverlay -Xfrontend -module-abi-name -Xfrontend os "${flags[@]}" "$here/overlay/os.swift" -o "$t/os.o"
swiftc -c -module-name Combine "${flags[@]}" "$here/overlay/Combine.swift" -o "$t/Combine.o"
# Brücke zu Darlings Foundation-Klassen (String._bridgeToObjectiveC usw.): ABI-Name Foundation
swiftc -c -module-name FoundationBridge -Xfrontend -module-abi-name -Xfrontend Foundation -import-objc-header "$here/overlay/bridge.h" "${flags[@]}" "$here/overlay/bridge.swift" -o "$t/bridge.o"
# Autolink-Marker, die Apples Overlays sonst liefern (nur der Name zählt)
cat > "$t/markers.c" <<'C'
char m1 __asm__("__swift_FORCE_LOAD_$_swiftos");
char m2 __asm__("__swift_FORCE_LOAD_$_swiftCompatibility56");
char m3 __asm__("__swift_FORCE_LOAD_$_swiftCompatibilityConcurrency");
char m4 __asm__("__swift_FORCE_LOAD_$_swiftDispatch");
char m5 __asm__("__swift_FORCE_LOAD_$_swiftXPC");
C
clang -arch arm64 -c "$t/markers.c" -o "$t/markers.o"
clang -arch arm64 -mmacosx-version-min=11.0 -fno-objc-arc -c -x objective-c "$here/overlay/compat.m" -o "$t/compat.o"
# ld braucht libSystem als Abhängigkeit; Attrappe mit anderem Namen, danach umbenannt
cat > "$t/sys.tbd" <<'T'
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
libs=()
for l in libswiftCore libswift_Concurrency libswiftSynchronization libswiftObservation libswift_StringProcessing libswift_RegexParser libswiftDarwin; do
  libs+=("$run/$l.dylib")
done
# libswiftCompatibilitySpan (Apples Rückportierung von Span) re-exportiert hier nur libswiftCore, die Span selbst enthält
ld -arch arm64 -dylib -o "$run/libswiftCompatibilitySpan.dylib" -install_name @rpath/libswiftCompatibilitySpan.dylib \
  -platform_version macos 11.0 11.0 -reexport_library "$run/libswiftCore.dylib" -not_for_dyld_shared_cache
ld -arch arm64 -dylib -o "$run/fo.dylib" \
  "$fb/FoundationEssentials.o" "$fb/DequeModule.o" "$fb/InternalCollectionsUtilities.o" "$fb/OrderedCollections.o" "$fb/_RopeModule.o" "$fb/_FoundationCShims.o" \
  "$t/os.o" "$t/Combine.o" "$t/markers.o" "$t/compat.o" "$t/bridge.o" "${libs[@]}" "$t/sys.tbd" \
  -platform_version macos 11.0 11.0 -install_name /usr/lib/sov/fo.dylib -rpath /usr/lib/swift -not_for_dyld_shared_cache -undefined dynamic_lookup -L"$(xcrun --show-sdk-path)/usr/lib" -lobjc
install_name_tool -change /usr/lib/libSystemX.B.dylib /usr/lib/libSystem.B.dylib "$run/fo.dylib"
echo "$run/fo.dylib"
