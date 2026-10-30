#!/bin/bash
# Stub-Frameworks für die geschlossenen Apple-Bibliotheken, die Mac-Programme importieren (Logic Pro),
# damit dyld sie laden kann. Einzelheiten: docs/logic-stubs.md.
#
#   macapps/stubs.sh [APP.app]                 x86_64, nach $WORK/out/macapps/root/…/MacFrameworks
#   CLOKEOS_ARCH=arm64 macapps/stubs.sh        arm64, nach $WORK/out/macapps-arm64/root/…
#
# Die Importliste liest collect.py aus APP.app (Standard /Applications/Logic Pro.app) und legt sie als
# $WORK/out/stubgen/imports.json ab; ist die Datei schon da und keine APP angegeben, wird sie benutzt.
# Braucht CoreFoundation aus foundation.sh (für die CFString-Konstanten).
set -euo pipefail
source "$(dirname "$0")/../config.sh"
CLOKEOS_FLAVOR=desktop
source "$(dirname "$0")/../config.sh"
need clang xcrun python3

S="$HERE/macapps/stubgen"
out="$WORK/out/macapps"
[ "$CLOKE_ARCH" = arm64 ] && out="$WORK/out/macapps-arm64"
gen="$WORK/out/stubgen/$CLOKE_ARCH"
imp="$WORK/out/stubgen/imports.json"
mkdir -p "$WORK/out/stubgen"
if [ $# -ge 1 ] || [ ! -f "$imp" ]; then
  "$S/collect.py" "${1:-/Applications/Logic Pro.app}" "$imp" >/dev/null
fi
[ -f "$out/obj/CoreFoundation" ] || { echo "Fehlt: $out/obj/CoreFoundation (zuerst macapps/foundation.sh)" >&2; exit 1; }
rm -rf "$gen/root"
# Was die schon gebauten Mac-Frameworks (Foundation, AppKit, …) aus den Stub-Bibliotheken importieren, muss es auch geben:
# ein Stub-Security ersetzt PureDarwins Security im Framework-Pfad ganz
"$S/collect.py" "$out/root/Library/ClokeOS/MacFrameworks" "$gen-macimports.json" --arch "$CLOKE_ARCH" >/dev/null
"$S/stubgen.py" "$imp" "$gen" --arch "$CLOKE_ARCH" --merge "$gen-macimports.json" --build "$out/pdlibs" "$out/obj/CoreFoundation"
mkdir -p "$out/root/Library/ClokeOS/MacFrameworks"
for f in "$gen"/root/Library/ClokeOS/MacFrameworks/*.framework; do
  rm -rf "$out/root/Library/ClokeOS/MacFrameworks/$(basename "$f")"
  cp -R "$f" "$out/root/Library/ClokeOS/MacFrameworks/"
done
# flache Bibliothek (libMobileGestalt): gehört nach /usr/lib
if [ -d "$gen/root/usr/lib" ]; then mkdir -p "$out/root/usr/lib"; cp "$gen"/root/usr/lib/*.dylib "$out/root/usr/lib/"; fi
echo "Fertig: $gen (Frameworks nach $out/root)"
