#!/bin/bash
# Legt Darlings Mac-Frameworks und die Beispiel-App Hallo in eine Stage für das Desktop-Image.
#
#   macapps/install.sh <stage>     (ruft brand.sh mit CLOKEOS_FLAVOR=desktop auf)
#
# Sind die Darling-Quellen geladen (~/ClokeOS-work/download/darling/, siehe README.md), baut es
# vorher mit foundation.sh und appkit.sh (beide bauen nur neu, was sich geändert hat). Sonst
# bleibt das Image ohne Mac-Frameworks, und CloakServer meldet bei Mac-Programmen mit Fenstern
# weiter, dass AppKit fehlt.
#
# Gebaut wird für die Architektur des Images (CLOKEOS_ARCH, x86_64 oder arm64); Hallo.app ist wie
# heutige Mac-Programme universal (x86_64 und arm64 in einer Datei, CloakServer und dyld nehmen den
# passenden Teil).
#
# In der Stage landen
#   /Library/ClokeOS/MacFrameworks/*.framework   CoreFoundation, Foundation, AppKit, Cocoa, …
#   /Applications/Hallo.app                      hallo.m als echtes Mac-Programm (universal, Apples SDK)
# CloakServer erkennt Programme, deren Frameworks in /Library/ClokeOS/MacFrameworks liegen, und
# startet sie mit DYLD_FRAMEWORK_PATH (cloakserver/apps.c). PureDarwins CoreFoundation, Onyx2D und
# CoreGraphics bleiben für alles andere.
set -euo pipefail
source "$(dirname "$0")/../config.sh"
CLOKEOS_FLAVOR=desktop
source "$(dirname "$0")/../config.sh"
[ $# -eq 1 ] || { echo "Aufruf: macapps/install.sh <stage>" >&2; exit 1; }
stage="$1"
M="$HERE/macapps"
D="$WORK/download/darling"
fw="$WORK/out/macapps/root/Library/ClokeOS/MacFrameworks"
[ "$CLOKE_ARCH" = arm64 ] && fw="$WORK/out/macapps-arm64/root/Library/ClokeOS/MacFrameworks"
for r in darling-corefoundation darling-foundation darling-cocotron; do
  if [ ! -d "$D/$r" ]; then
    echo "Mac-Frameworks: ohne $D/$r, das Image bleibt ohne AppKit"
    exit 0
  fi
done
"$M/foundation.sh" | tail -1
"$M/appkit.sh" | tail -1
# Stub-Frameworks für geschlossene Apple-Bibliotheken (Logic Pro), sobald stubs.sh einmal eine Importliste angelegt hat
[ -f "$WORK/out/stubgen/imports.json" ] && { "$M/stubs.sh" | tail -1 || true; }
"$M/coreaudio.sh" | tail -2 | head -1 || echo "CoreAudio-Shim nicht gebaut"
"$M/audiotoolbox.sh" | tail -2 | head -1 || echo "AudioToolbox-Teilmenge nicht gebaut"
"$M/accelerate.sh" | tail -2 | head -1 || echo "Accelerate-Teilmenge nicht gebaut"
"$M/libs.sh" | tail -1 || echo "libsqlite3/libxml2/libbz2/libpcre/libxar nicht gebaut"
"$M/pkginstall.sh" | tail -1 || echo "clokeos-pkginstall nicht gebaut"

# Swift-Symbole, die Mac-Programme importieren und die hier fehlen: Trap-Marken in usr/lib/sov, per Re-Export an den Bibliotheken
# (macapps/stubgen/swiftov.py; Voraussetzung ist die Importliste von stubs.sh). Vor dem Kopieren in die Stage, die Bibliotheken
# werden im Ausgabeordner gepatcht.
CLOKE_ARCH_CC=$([ "$CLOKE_ARCH" = arm64 ] && echo arm64 || echo x86_64)
# die echte Swift-Laufzeit (swift/build.sh) ersetzt die leeren Stubs, bevor swiftov sie patcht
if [ -f "$WORK/swiftsrc/run-$CLOKE_ARCH/libswiftCore.dylib" ]; then
  mkdir -p "${fw%/Library/ClokeOS/MacFrameworks}/usr/lib/swift"
  cp "$WORK/swiftsrc/run-$CLOKE_ARCH/libswiftCore.dylib" "$WORK/swiftsrc/run-$CLOKE_ARCH/libswift_Concurrency.dylib" "${fw%/Library/ClokeOS/MacFrameworks}/usr/lib/swift/"
fi
if [ -f "$WORK/out/stubgen/imports.json" ]; then
  python3 -I "$M/stubgen/swiftov.py" "$WORK/out/stubgen/imports.json" "${fw%/Library/ClokeOS/MacFrameworks}" "${fw%/Library/ClokeOS/MacFrameworks}" --arch "$CLOKE_ARCH_CC" \
    --build "${fw%/root/Library/ClokeOS/MacFrameworks}/pdlibs" "${fw%/root/Library/ClokeOS/MacFrameworks}/obj/CoreFoundation" \
    --pd-image "$DEBUGFS" "$PD_IMG?offset=$(( $(gpt_part "$PD_IMG" "$GUID_LINUX_FS" | cut -d' ' -f1) * 512 ))" | tail -1 || true
fi
mkdir -p "$stage/Library/ClokeOS"
rm -rf "$stage/Library/ClokeOS/MacFrameworks"
cp -R "$fw" "$stage/Library/ClokeOS/MacFrameworks"   # mit den Symlinks der Bundles
rootdir="${fw%/Library/ClokeOS/MacFrameworks}"
if [ -f "$rootdir/usr/lib/libMobileGestalt.dylib" ]; then   # flache Stub-Bibliothek aus stubs.sh
  mkdir -p "$stage/usr/lib"; cp "$rootdir/usr/lib/libMobileGestalt.dylib" "$stage/usr/lib/"
fi
if [ -d "$rootdir/usr/lib/swift" ]; then   # leere Swift-Stubs aus stubs.sh (echte: swift/build.sh)
  mkdir -p "$stage/usr/lib/swift"; cp "$rootdir"/usr/lib/swift/*.dylib "$stage/usr/lib/swift/"
fi
[ -f "$rootdir/usr/lib/libAppleArchive.dylib" ] && { mkdir -p "$stage/usr/lib"; cp "$rootdir/usr/lib/libAppleArchive.dylib" "$stage/usr/lib/"; }
# Echte Swift-Laufzeit, wenn swift/build.sh sie gebaut hat ($WORK/swiftsrc/run-<arch>): ersetzt die leeren Stubs von oben;
# dazu die libdispatch-Datei, die libswift_Concurrency mit festem Pfad lädt (swift/dispatch-shim.c)
sw="$WORK/swiftsrc/run-$CLOKE_ARCH"
if [ -f "$sw/libswiftCore.dylib" ]; then
  mkdir -p "$stage/usr/lib/swift" "$stage/usr/lib/system"
  # Mit ld statt clang: das SDK hat keine x86_64-Scheibe der System-Bibliotheken. Die tbd-Attrappe heißt anders, damit ld nicht
  # das SDK-libSystem nimmt; install_name_tool setzt den Re-Export danach auf die echte libSystem.
  la="$WORK/tmp/dispatch-shim"; mkdir -p "$la"
  printf -- "--- !tapi-tbd\ntbd-version: 4\ntargets: [ x86_64-macos, arm64-macos ]\ninstall-name: '/usr/lib/libSystemX.B.dylib'\ncurrent-version: 1292\ncompatibility-version: 1\nexports:\n  - targets: [ x86_64-macos, arm64-macos ]\n    symbols: [ dummy_sym ]\n...\n" > "$la/sysx.tbd"
  clang -arch "$CLOKE_ARCH_CC" -c "$HERE/swift/dispatch-shim.c" -o "$la/ds.o" &&
  ld -arch "$CLOKE_ARCH_CC" -dylib -o "$stage/usr/lib/system/libdispatch.dylib" "$la/ds.o" -install_name /usr/lib/system/libdispatch.dylib \
    -platform_version macos 11.0 11.0 -reexport_library "$la/sysx.tbd" 2>/dev/null &&
  install_name_tool -change /usr/lib/libSystemX.B.dylib /usr/lib/libSystem.B.dylib "$stage/usr/lib/system/libdispatch.dylib"
fi
# Neu gebautes libobjc (libobjc/build.sh): nur auf Wunsch (CLOKE_LIBOBJC=1, macapps/logicimage.sh setzt es), arm64
if [ "${CLOKE_LIBOBJC:-}" = 1 ]; then
  "$HERE/libobjc/build.sh" > /dev/null && { mkdir -p "$stage/usr/lib"; cp "$WORK/out/libobjc-$CLOKE_ARCH/libobjc.A.dylib" "$stage/usr/lib/libobjc.A.dylib"; } || echo "libobjc nicht gebaut"
fi
[ -f "$rootdir/usr/lib/libIOKitCF.dylib" ] && { mkdir -p "$stage/usr/lib"; cp "$rootdir/usr/lib/libIOKitCF.dylib" "$stage/usr/lib/libIOKitCF.dylib"; }
[ -d "$rootdir/usr/lib/sov" ] && { mkdir -p "$stage/usr/lib/sov"; cp "$rootdir"/usr/lib/sov/*.dylib "$stage/usr/lib/sov/"; }
# Kleine C-Bibliotheken mit Apples Installnamen (libs.sh) und der Paketinstaller (pkginstall.sh, docs/logic-installer.md)
for l in libsqlite3.dylib libxml2.2.dylib libxml2.dylib libbz2.1.0.dylib libbz2.dylib libpcre.0.dylib libpcre.dylib libxar.1.dylib libxar.dylib; do
  [ -e "$rootdir/usr/lib/$l" ] && { mkdir -p "$stage/usr/lib"; cp -P "$rootdir/usr/lib/$l" "$stage/usr/lib/"; }
done
[ -f "$rootdir/usr/sbin/clokeos-pkginstall" ] && { mkdir -p "$stage/usr/sbin"; cp "$rootdir/usr/sbin/clokeos-pkginstall" "$stage/usr/sbin/"; }

# MacLibs: PureDarwins /usr/lib/libCoreFoundation.dylib und libFoundation.dylib definieren dieselben Klassen wie Darlings Frameworks
# (__NSCFNumber, __NSCFType, NSNull …); die PureDarwin-Fassung kommt zuerst dran und hat nicht, was ein Mac-Programm braucht
# (z. B. -[NSNumber objCType]). Ein Programm, das mit DYLD_LIBRARY_PATH=/Library/ClokeOS/MacLibs läuft, bekommt dort statt dessen
# Darlings Frameworks (dyld ersetzt nach dem Dateinamen). Nur für Programme, nicht für Systemdienste. docs/logic-launch.md, Runde 13.
mkdir -p "$stage/Library/ClokeOS/MacLibs"
ln -sf ../MacFrameworks/CoreFoundation.framework/Versions/A/CoreFoundation "$stage/Library/ClokeOS/MacLibs/libCoreFoundation.dylib"
ln -sf ../MacFrameworks/Foundation.framework/Versions/C/Foundation "$stage/Library/ClokeOS/MacLibs/libFoundation.dylib"

# Hallo.app, aufgebaut wie ein Programm aus Xcode (Info.plist, PkgInfo, MacOS, Resources)
app="$stage/Applications/Hallo.app/Contents"
rm -rf "$stage/Applications/Hallo.app"
mkdir -p "$app/MacOS" "$app/Resources"
clang -arch x86_64 -arch arm64 -mmacosx-version-min=11.0 -fobjc-arc -O2 -framework Cocoa -o "$app/MacOS/hallo" "$M/hallo.m"
cp "$M/hallo-icon.svg" "$app/Resources/AppIcon.svg"
printf 'APPL????' > "$app/PkgInfo"
cat > "$app/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>de</string>
	<key>CFBundleDisplayName</key><string>Hallo</string>
	<key>CFBundleExecutable</key><string>hallo</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>CFBundleIdentifier</key><string>com.clokeos.hallo</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>Hallo</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
	<key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
EOF
echo "Mac-Frameworks: $(ls "$fw" | wc -l | tr -d ' ') Frameworks und Hallo.app in der Stage"
