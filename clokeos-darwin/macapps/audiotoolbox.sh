#!/bin/bash
# AudioToolbox-Shim: baut macapps/audiotoolbox/*.c als AudioToolbox.framework für ClokeOS, dazu das Prüfprogramm
# attest und das Test-Plug-in ctgain.component (AUv2, nur zum Testen des Komponenten-Registers).
#
#   macapps/audiotoolbox.sh                         x86_64, nach ~/ClokeOS-work/out/macapps/root/
#   CLOKEOS_ARCH=arm64 macapps/audiotoolbox.sh      arm64, nach ~/ClokeOS-work/out/macapps-arm64/root/
#
# Braucht CoreFoundation (foundation.sh) und CoreAudio.framework (coreaudio.sh, wird für den Ausgabe-Unit gelinkt).
# Übersetzt wird mit Apples clang und SDK-Headern, gelinkt gegen PureDarwins libSystem wie bei coreaudio.sh.
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
ca="$root$fwdir/CoreAudio.framework/Versions/A/CoreAudio"
[ -f "$ca" ] || { echo "Fehlt: $ca (zuerst macapps/coreaudio.sh)" >&2; exit 1; }

d="$root$fwdir/AudioToolbox.framework"
rm -rf "$d"; mkdir -p "$d/Versions/A/Resources"
# ALAC-Decoder (Apples Quelltext, Apache 2.0, macapps/audiotoolbox/alac) und seine C-Schnittstelle: C++ ohne Ausnahmen/RTTI, braucht kein libc++
alacobj="$obj/alac"; mkdir -p "$alacobj"
for f in ALACDecoder.cpp ../alac_glue.cpp; do
  clang++ -target "$TARGET" -isysroot "$SDK" -O2 -g0 -w -fno-exceptions -fno-rtti ${ARCHCC[@]+"${ARCHCC[@]}"} -c "$M/audiotoolbox/alac/$f" -o "$alacobj/$(basename "${f%.cpp}").o"
done
alacsrc=("$M"/audiotoolbox/alac/{ag_dec,dp_dec,matrix_dec,ALACBitUtilities,EndianPortable}.c)
clang -target "$TARGET" -isysroot "$SDK" -O2 -g0 -Wno-deprecated-declarations ${ARCHCC[@]+"${ARCHCC[@]}"} -dynamiclib -nostdlib \
  -Wl,-platform_version,macos,11.0,11.5 -Wl,-syslibroot,"$pd" \
  ${CLOKE_ARCH:+$([ "$CLOKE_ARCH" = arm64 ] && echo -Wl,-no_fixup_chains)} \
  -install_name /System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox \
  -compatibility_version 1 -current_version 1 \
  -o "$d/Versions/A/AudioToolbox" "$M/audiotoolbox/AudioToolbox.c" "$M/audiotoolbox/convert.c" "$M/audiotoolbox/file.c" \
  "${alacsrc[@]}" "$alacobj/ALACDecoder.o" "$alacobj/alac_glue.o" -w \
  "$obj/CoreFoundation" "$ca" "$pd/usr/lib/libSystem.B.dylib" 2>&1 | grep -v '^ld: warning' || true
[ -f "$d/Versions/A/AudioToolbox" ] || exit 1
cat > "$d/Versions/A/Resources/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.apple.audio.toolbox.AudioToolbox</string>
<key>CFBundleName</key><string>AudioToolbox</string>
<key>CFBundleExecutable</key><string>AudioToolbox</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleVersion</key><string>1</string>
</dict></plist>
PL
ln -s A "$d/Versions/Current"; ln -s Versions/Current/AudioToolbox "$d/AudioToolbox"; ln -s Versions/Current/Resources "$d/Resources"

# Prüfprogramm und Test-Plug-in gegen Apples SDK gelinkt (wie Mac-Programme), zur Laufzeit gilt unser Framework
p="$root/Library/ClokeOS/Probe"; mkdir -p "$p/Components/ctgain.component/Contents/MacOS"
clang -target "$TARGET" -isysroot "$SDK" -O2 -Wno-deprecated-declarations ${ARCHCC[@]+"${ARCHCC[@]}"} -framework AudioToolbox -framework CoreFoundation -framework Foundation \
  -o "$p/attest" "$M/audiotoolbox/attest.c" -lm
clang -target "$TARGET" -isysroot "$SDK" -O2 -w ${ARCHCC[@]+"${ARCHCC[@]}"} -bundle -framework AudioToolbox \
  -o "$p/Components/ctgain.component/Contents/MacOS/ctgain" "$M/audiotoolbox/ctgain.c"
cat > "$p/Components/ctgain.component/Contents/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>ctgain</string>
<key>CFBundleIdentifier</key><string>tech.cloke.ctgain</string>
<key>AudioComponents</key><array><dict>
<key>type</key><string>aufx</string><key>subtype</key><string>gain</string><key>manufacturer</key><string>Clok</string>
<key>name</key><string>Clok: Test Gain</string><key>version</key><integer>65536</integer>
<key>factoryFunction</key><string>CtGainFactory</string>
</dict></array></dict></plist>
PL
cat > "$p/at.sh" <<'EOF'
#!/bin/zsh
# AudioToolbox-Probe: Dateien, Wandler, Komponentenregister, Ausgabe-Unit (zsh at.sh self | play [Sekunden])
export DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks
export CLOKE_AU_PATH=/Library/ClokeOS/Probe/Components
export CLOKE_CA_STATS=1
cd /tmp
/Library/ClokeOS/Probe/attest "$@"
echo "attest-Ende: $?"
EOF
chmod +x "$p/at.sh"
echo "Fertig: $d"
nm -gU "$d/Versions/A/AudioToolbox" | awk '{print $3}' | tr '\n' ' '; echo
