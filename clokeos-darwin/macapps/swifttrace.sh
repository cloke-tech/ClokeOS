#!/bin/bash
# Objektspur für Swift-Programme auf ClokeOS (docs/logic-launch.md, Runde 28): legt vor die Swift-Laufzeit einen Wrapper (macapps/swifttrace.c).
#   macapps/swifttrace.sh <Ausgabeordner> <echtes libswiftCore.dylib>        baut <Ordner>/usr/lib/swift/{libswiftCore,libswiftCoreReal}.dylib
# Beides ins Image schreiben (die echte Datei bekommt den Namen libswiftCoreReal.dylib); SWIFT_TRACE_CLASS=<Teil des Klassennamens> in der Umgebung des Programms.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$1"; real="$2"; d="$out/usr/lib/swift"
mkdir -p "$d"
cp "$real" "$d/libswiftCoreReal.dylib"
install_name_tool -id @rpath/libswiftCoreReal.dylib "$d/libswiftCoreReal.dylib"
clang -arch arm64 -mmacosx-version-min=11.0 -dynamiclib -O1 -install_name @rpath/libswiftCore.dylib -Wl,-not_for_dyld_shared_cache \
  -Wl,-reexport_library,"$d/libswiftCoreReal.dylib" -Wl,-rpath,/usr/lib/swift -lobjc -o "$d/libswiftCore.dylib" "$here/swifttrace.c" 2>&1 | grep -v warning || true
echo "$d/libswiftCore.dylib"
