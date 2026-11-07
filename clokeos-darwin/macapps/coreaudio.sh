#!/bin/bash
# CoreAudio-Shim: baut macapps/coreaudio/CoreAudio.c als CoreAudio.framework für ClokeOS und das
# Prüfprogramm satest (2 s Sinus über die öffentliche HAL).
#
#   macapps/coreaudio.sh                         x86_64, nach ~/ClokeOS-work/out/macapps/root/
#   CLOKEOS_ARCH=arm64 macapps/coreaudio.sh      arm64, nach ~/ClokeOS-work/out/macapps-arm64/root/
#
# Braucht CoreFoundation aus foundation.sh (für CFString-Eigenschaften). Übersetzt wird mit Apples
# clang und SDK-Headern, gelinkt gegen PureDarwins libSystem wie bei foundation.sh.
# Danach swift/deploy-overlay.sh: swiftov hängt die Symbole an, die Logic sonst noch aus CoreAudio importiert
# (Re-Export von usr/lib/sov/…); ohne den Schritt bricht dyld bei _AudioConvertNanosToHostTime ab.
# Test ohne Gerät: CLOKE_CA_DEVICE=<datei> schreibt WAV im Echtzeittakt, CLOKE_CA_TRACE=1 meldet Start/IOProcs/unbekannte Eigenschaften.
set -euo pipefail
source "$(dirname "$0")/../config.sh"
CLOKEOS_FLAVOR=desktop
source "$(dirname "$0")/../config.sh"
need clang xcrun

M="$HERE/macapps"
SDK="$(xcrun --show-sdk-path)"
TARGET="$CLOKE_ARCH-apple-macos11.0"
out="$WORK/out/macapps"; ARCHCC=()
[ "$CLOKE_ARCH" = arm64 ] && { out="$WORK/out/macapps-arm64"; ARCHCC=(-fno-stack-check); }
obj="$out/obj"; root="$out/root"; pd="$out/pdlibs"
fwdir="/Library/ClokeOS/MacFrameworks"
[ -f "$obj/CoreFoundation" ] || { echo "Fehlt: $obj/CoreFoundation (zuerst macapps/foundation.sh)" >&2; exit 1; }

d="$root$fwdir/CoreAudio.framework"
rm -rf "$d"; mkdir -p "$d/Versions/A/Resources"
clang -target "$TARGET" -isysroot "$SDK" -O2 -g0 -w ${ARCHCC[@]+"${ARCHCC[@]}"} -dynamiclib -nostdlib \
  -Wl,-platform_version,macos,11.0,11.5 -Wl,-syslibroot,"$pd" \
  ${CLOKE_ARCH:+$([ "$CLOKE_ARCH" = arm64 ] && echo -Wl,-no_fixup_chains)} \
  -install_name /System/Library/Frameworks/CoreAudio.framework/Versions/A/CoreAudio \
  -compatibility_version 1 -current_version 1 \
  -o "$d/Versions/A/CoreAudio" "$M/coreaudio/CoreAudio.c" \
  "$obj/CoreFoundation" "$pd/usr/lib/libSystem.B.dylib" 2>&1 | grep -v '^ld: warning' || true
[ -f "$d/Versions/A/CoreAudio" ] || exit 1
cat > "$d/Versions/A/Resources/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.apple.audio.CoreAudio</string>
<key>CFBundleName</key><string>CoreAudio</string>
<key>CFBundleExecutable</key><string>CoreAudio</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleVersion</key><string>1</string>
</dict></plist>
PL
ln -s A "$d/Versions/Current"; ln -s Versions/Current/CoreAudio "$d/CoreAudio"; ln -s Versions/Current/Resources "$d/Resources"

# Prüfprogramm gegen Apples SDK gelinkt (wie ein Mac-Programm), zur Laufzeit gilt unser Framework
mkdir -p "$root/Library/ClokeOS/Probe"
clang -target "$TARGET" -isysroot "$SDK" -O2 ${ARCHCC[@]+"${ARCHCC[@]}"} -framework CoreAudio \
  -o "$root/Library/ClokeOS/Probe/satest" "$M/coreaudio/satest.c"
echo "Fertig: $d"
nm -gU "$d/Versions/A/CoreAudio" | awk '{print $3}' | tr '\n' ' '; echo
