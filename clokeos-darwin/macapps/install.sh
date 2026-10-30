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
"$M/accelerate.sh" | tail -2 | head -1 || echo "Accelerate-Teilmenge nicht gebaut"

mkdir -p "$stage/Library/ClokeOS"
rm -rf "$stage/Library/ClokeOS/MacFrameworks"
cp -R "$fw" "$stage/Library/ClokeOS/MacFrameworks"   # mit den Symlinks der Bundles
rootdir="${fw%/Library/ClokeOS/MacFrameworks}"
if [ -f "$rootdir/usr/lib/libMobileGestalt.dylib" ]; then   # flache Stub-Bibliothek aus stubs.sh
  mkdir -p "$stage/usr/lib"; cp "$rootdir/usr/lib/libMobileGestalt.dylib" "$stage/usr/lib/"
fi

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
