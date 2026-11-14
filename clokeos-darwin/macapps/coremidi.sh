#!/bin/bash
# CoreMIDI für ClokeOS: baut macapps/coremidi/CoreMIDI.c als CoreMIDI.framework (ersetzt das Stub-Framework aus
# stubs.sh) und das Prüfprogramm mtest (gegen Apples SDK gelinkt, Ausgabe muss der von Apples CoreMIDI gleichen).
#
#   macapps/coremidi.sh                         x86_64, nach ~/ClokeOS-work/out/macapps/root/
#   CLOKEOS_ARCH=arm64 macapps/coremidi.sh      arm64, nach ~/ClokeOS-work/out/macapps-arm64/root/
#
# Braucht CoreFoundation aus foundation.sh. Kein MIDI-Server, keine Hardware: virtuelle Quellen und Ziele im Prozess.
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

d="$root$fwdir/CoreMIDI.framework"
rm -rf "$d"; mkdir -p "$d/Versions/A/Resources"
clang -target "$TARGET" -isysroot "$SDK" -O2 -g0 -fblocks -fvisibility=hidden -Wno-deprecated-declarations -Wno-availability \
  ${ARCHCC[@]+"${ARCHCC[@]}"} -dynamiclib -nostdlib \
  -Wl,-platform_version,macos,11.0,11.5 -Wl,-syslibroot,"$pd" \
  ${CLOKE_ARCH:+$([ "$CLOKE_ARCH" = arm64 ] && echo -Wl,-no_fixup_chains)} \
  -install_name /System/Library/Frameworks/CoreMIDI.framework/Versions/A/CoreMIDI \
  -compatibility_version 1 -current_version 69 \
  -o "$d/Versions/A/CoreMIDI" "$M/coremidi/CoreMIDI.c" \
  "$obj/CoreFoundation" "$pd/usr/lib/libSystem.B.dylib" 2>&1 | grep -v '^ld: warning' || true
[ -f "$d/Versions/A/CoreMIDI" ] || exit 1
cat > "$d/Versions/A/Resources/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.apple.audio.midi.CoreMIDI</string>
<key>CFBundleName</key><string>CoreMIDI</string>
<key>CFBundleExecutable</key><string>CoreMIDI</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleVersion</key><string>69</string>
</dict></plist>
PL
ln -s A "$d/Versions/Current"; ln -s Versions/Current/CoreMIDI "$d/CoreMIDI"; ln -s Versions/Current/Resources "$d/Resources"

# Prüfprogramm gegen Apples SDK (wie ein Mac-Programm), zur Laufzeit gilt unser Framework
p="$root/Library/ClokeOS/Probe"; mkdir -p "$p"
clang -target "$TARGET" -isysroot "$SDK" -O1 -w ${ARCHCC[@]+"${ARCHCC[@]}"} -framework CoreMIDI -framework CoreFoundation -framework Foundation \
  -o "$p/mtest" "$M/coremidi/mtest.c"
echo "Fertig: $d"
