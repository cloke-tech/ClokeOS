#!/bin/bash
# Baut die eigenen Kernel-Treiber von ClokeOS (kext/<Name>/: *.cpp und Info.plist) und daraus mit
# PureDarwins kc-builder den Kernel Collection für die EFI-Partition (EFI/BOOT/kernel), x86_64.
#
#   kext/build.sh            Ergebnis: ~/ClokeOS-work/out/kexts/<Name>.kext und ~/ClokeOS-work/out/kernel
#
# Header (Kernel.framework), Kernel, PureDarwins Treiber, kc-builder und die Liste der Treiber
# (pd-rebuild-kc) kommen mit debugfs aus dem Desktop-Image, ohne es zu booten. kc-builder ist ein
# x86_64-Programm, das nur libSystem braucht; auf einem Mac mit Apple-Chip läuft es über Rosetta.
# Ohne eigene Treiber ist das Ergebnis byte-gleich mit dem Kernel aus PureDarwins EFI-Partition
# (nach kcfix.py). Die Treiber werden wie PureDarwins eigene gelinkt: MH_BUNDLE gegen libSystem,
# Kernelsymbole per dynamic_lookup, Zeiger als Rebase-/Bind-Anweisungen (kc-builder wendet klassische
# lokale Relokationen nicht an).
set -euo pipefail
export CLOKEOS_FLAVOR=desktop
CLOKEOS_ARCH=x86_64
export CLOKEOS_ARCH
source "$(dirname "$0")/../config.sh"
need "$DEBUGFS" clang clang++ python3
src="$HERE/kext"
sdk=$(xcrun --show-sdk-path)

read -r root_start _ < <(gpt_part "$PD_IMG" "$GUID_LINUX_FS")
fs="$PD_IMG?offset=$((root_start * 512))"
base="$WORK/sysroot/kc-${PD_SHA256:0:12}"
if [ ! -f "$base/.fertig" ]; then
  rm -rf "$base"
  mkdir -p "$base/headers" "$base/root" "$base/avail"
  "$DEBUGFS" -R "rdump /System/Library/Frameworks/Kernel.framework/Versions/A/Headers $base/headers" "$fs" >/dev/null 2>&1
  "$DEBUGFS" -R "rdump /System/Library/Extensions $base/root" "$fs" >/dev/null 2>&1
  "$DEBUGFS" -R "rdump /System/Library/Kernels $base/root" "$fs" >/dev/null 2>&1
  "$DEBUGFS" -R "dump /usr/bin/kc-builder $base/kc-builder" "$fs" >/dev/null 2>&1
  "$DEBUGFS" -R "dump /usr/sbin/pd-rebuild-kc $base/pd-rebuild-kc" "$fs" >/dev/null 2>&1
  "$DEBUGFS" -R "dump /usr/lib/libSystem.B.dylib $base/libSystem.B.dylib" "$fs" >/dev/null 2>&1
  # Availability.h und Co. fehlen im Kernel.framework (bei Apple kommen sie über -isysroot)
  for h in Availability.h AvailabilityInternal.h AvailabilityMacros.h AvailabilityVersions.h TargetConditionals.h; do
    "$DEBUGFS" -R "dump /usr/include/$h $base/avail/$h" "$fs" >/dev/null 2>&1
  done
  chmod +x "$base/kc-builder"
  for f in headers/Headers/IOKit/IOService.h root/Kernels/kernel root/Extensions/System.kext/Info.plist kc-builder \
           pd-rebuild-kc libSystem.B.dylib avail/Availability.h; do
    [ -s "$base/$f" ] || { echo "Nicht aus dem Image geholt: $f" >&2; exit 1; }
  done
  touch "$base/.fertig"
fi
hdr="$base/headers/Headers"

# --- Treiber übersetzen ------------------------------------------------------------------------
out="$WORK/out/kexts"
tmp="$WORK/tmp/kexts"
rm -rf "$out" "$tmp"
mkdir -p "$out" "$tmp"
cflags=(-target x86_64-apple-macos11 -mkernel -fno-builtin -fno-common -nostdinc -I"$hdr" -idirafter "$base/avail"
        -DKERNEL -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type
        -ffile-prefix-map="$HERE"=clokeos-darwin -ffile-prefix-map="$WORK"=work)
kexts=()
for dir in "$src"/*/; do
  [ -f "$dir/Info.plist" ] || continue
  name=$(basename "$dir")
  # kc-builder liest die Info.plist mit einem eigenen Parser, der nach einem XML-Kommentar den Rest des
  # Wörterbuchs verliert (der Treiber hatte dann keine Match-Schlüssel und wurde nie zugeordnet)
  ! grep -q '<!--' "$dir/Info.plist" || { echo "$dir/Info.plist: keine XML-Kommentare (kc-builder)" >&2; exit 1; }
  id=$(plutil -extract CFBundleIdentifier raw "$dir/Info.plist")
  ver=$(plutil -extract CFBundleVersion raw "$dir/Info.plist")
  objs=()
  # ClokeI2CHID malt seine Schritte auf den Bildschirm (ClokeTrace aus kext/pdacpi, mit eigenen Namen, damit
  # sich die Symbole nicht mit denen der ACPI-Plattform beißen), um zu sehen, wo ein Hänger sitzt
  extra=()
  xflags=()
  if [ "$name" = ClokeRtw88 ]; then
    # Firmware als C-Feld einbauen (kein Dateisystem nötig, wenn der Treiber startet)
    fetch_file "$RTW8821C_FW_URL" "$RTW8821C_FW_SHA256" "$RTW8821C_FW" >/dev/null
    mkdir -p "$tmp/rtw88"
    python3 -I - "$RTW8821C_FW" "$tmp/rtw88/rtw8821c_fw.h" <<'PYFW'
import sys
d = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('/* rtw88/rtw8821c_fw.bin aus linux-firmware, Realtek (LICENCE.rtlwifi_firmware.txt) */\n')
    f.write('static const unsigned char rtw8821c_fw[%d] = {\n' % len(d))
    for i in range(0, len(d), 16):
        f.write(','.join('0x%02x' % b for b in d[i:i + 16]) + ',\n')
    f.write('};\n')
PYFW
    xflags=(-I"$tmp/rtw88")
  fi
  if [ "$name" = ClokeI2CHID ]; then
    extra=("$src/pdacpi/ClokeTrace.cpp")
    xflags=(-Dcloke_trace=i2c_trace -Dcloke_trace_show=i2c_trace_show -I"$src/pdacpi")
  fi
  for c in "$dir"/*.cpp ${extra[@]+"${extra[@]}"}; do
    o="$tmp/$name-$(basename "$c" .cpp).o"
    clang++ "${cflags[@]}" ${xflags[@]+"${xflags[@]}"} -fapple-kext -std=gnu++17 -fno-exceptions -fno-rtti -nostdinc++ -c "$c" -o "$o"
    objs+=("$o")
  done
  cat > "$tmp/$name-info.c" <<EOF
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($id, "$ver", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
EOF
  clang "${cflags[@]}" -c "$tmp/$name-info.c" -o "$tmp/$name-info.o"
  k="$out/$name.kext"
  mkdir -p "$k/Contents/MacOS"
  cp "$dir/Info.plist" "$k/Contents/Info.plist"
  clang++ -target x86_64-apple-macos11 -bundle -undefined dynamic_lookup -nostdlib -Wl,-no_fixup_chains \
    -L"$sdk/usr/lib" "${objs[@]}" "$tmp/$name-info.o" -lkmodc++ -lkmod "$base/libSystem.B.dylib" \
    -o "$k/Contents/MacOS/$name" 2> >(grep -v 'built for newer' >&2)
  kexts+=("$k")
  echo "Treiber: $k"
done

# --- RavynHDAudio mit Verstärker-Patch ----------------------------------------------------------
# PureDarwins Audiotreiber wird aus der Quelle neu gebaut (kext/audio/ravynhda-amp.patch): ohne Änderung
# programmiert er Verstärkung 0 (Minimum, nicht 0 dB) und überschreibt sie am Pin, /dev/dsp0 bleibt stumm.
# Der Patch verkleinert auch den Stapelpuffer in dsp0_write auf 2 KB: ab 4 KB ruft clang ___chkstk_darwin
# auf, das es im Kernel nicht gibt (erster Schreibzugriff = GP-Fault). Ersetzt PureDarwins Kext gleichen Namens.
fetch_file "$PDSRC_URL" "$PDSRC_SHA256" "$PDSRC_TGZ" >/dev/null
hda="$WORK/tmp/hda-src"
rm -rf "$hda"
mkdir -p "$hda"
tar -xzf "$PDSRC_TGZ" -C "$hda" --strip-components=1 '*/src/Kernel/Extensions/RavynHDAudio' \
  '*/src/Kernel/Extensions/IOAudioFamily/*.h' '*/src/Kernel/Extensions/IOAudioFamily/IOKit' '*/src/Kernel/Extensions/IOPCIFamily/include'
patch -s -p1 -d "$hda" < "$src/audio/ravynhda-amp.patch"
hdir="$hda/src/Kernel/Extensions/RavynHDAudio"
hobjs=()
for c in "$hdir"/*.cpp; do
  o="$tmp/hda-$(basename "$c" .cpp).o"
  clang++ "${cflags[@]}" -I"$hda/src/Kernel/Extensions/IOAudioFamily" -I"$hda/src/Kernel/Extensions/IOPCIFamily/include" -fapple-kext -std=gnu++17 -fno-exceptions -fno-rtti -nostdinc++ -c "$c" -o "$o"
  hobjs+=("$o")
done
stock="$base/root/Extensions/RavynHDAudio.kext/Contents/Info.plist"
hid=$(plutil -extract CFBundleIdentifier raw "$stock")
hver=$(plutil -extract CFBundleVersion raw "$stock")
cat > "$tmp/hda-info.c" <<EOF
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($hid, "$hver", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
EOF
clang "${cflags[@]}" -c "$tmp/hda-info.c" -o "$tmp/hda-info.o"
k="$out/RavynHDAudio.kext"
mkdir -p "$k/Contents/MacOS"
cp "$stock" "$k/Contents/Info.plist"
# Echte Rechner: jeder HD-Audio-Controller nach Klasse (0x040300) statt der drei Intel-Kennungen der Vorlage
# (QEMU und Intel sind darin enthalten); der Treiber prüft die Klasse selbst (RavynHDAudio::probe).
# Gebraucht für AMD (1022:1487). Die drei alten Einträge entfallen, damit der Kernel nicht größer wird.
for pn in GeminiLake ICH9HDA IntelHDA; do
  plutil -remove "IOKitPersonalities.RavynHDAudio-$pn" "$k/Contents/Info.plist" 2>/dev/null || true
done
plutil -insert IOKitPersonalities.RavynHDAudio-HDAClass -json \
  "{\"CFBundleIdentifier\":\"$hid\",\"IOClass\":\"RavynHDAudio\",\"IOProviderClass\":\"IOPCIDevice\",\"IOPCIClassMatch\":\"0x04030000&0xffffff00\",\"IOProbeScore\":5000}" \
  "$k/Contents/Info.plist"
clang++ -target x86_64-apple-macos11 -bundle -undefined dynamic_lookup -nostdlib -Wl,-no_fixup_chains \
  -L"$sdk/usr/lib" "${hobjs[@]}" "$tmp/hda-info.o" -lkmodc++ -lkmod "$base/libSystem.B.dylib" \
  -o "$k/Contents/MacOS/RavynHDAudio" 2> >(grep -v 'built for newer' >&2)
if nm -u "$k/Contents/MacOS/RavynHDAudio" | grep -q chkstk_darwin; then
  echo "RavynHDAudio ruft ___chkstk_darwin (Stapelrahmen >= 4 KB)" >&2; exit 1
fi
kexts+=("$k")
echo "Treiber: $k"

# --- PureDarwins PDRealtek8111 und IOUSBHIDDriver mit ClokeOS-Patches -------------------------------
# PDRealtek8111 (kext/net/pdrealtek8111-rx.patch): größerer Empfangsring, Empfangsteil nach einem Überlauf neu
# aufsetzen, ASPM aus; auf einem Ryzen-PC stand der Empfang bei Last (etwa 1 MB am Stück) für immer still.
# IOUSBHIDDriver (kext/usbhid/iousbhid-stall.patch): eine gestoppte Interrupt-Pipe wird freigegeben und mit
# wachsender Wartezeit neu gelesen; vorher lief die Schleife endlos (~140 Meldungen pro Sekunde im Kernel-Log).
rebuilt=(RavynHDAudio.kext)
rebuild_kext() { # Name Unterordner Patch Muster-im-Archiv...
  local name="$1" sub="$2" patchf="$3"; shift 3
  local d="$WORK/tmp/$name-src" o objs=() ext
  rm -rf "$d"; mkdir -p "$d"
  tar -xzf "$PDSRC_TGZ" -C "$d" --strip-components=1 "$@"
  patch -s -p1 -d "$d/src/Kernel/Extensions/$sub" < "$patchf"
  ext="$d/src/Kernel/Extensions/$sub"
  for c in "$ext"/*.cpp; do
    o="$tmp/$name-$(basename "$c" .cpp).o"
    clang++ "${cflags[@]}" -I"$d/src/Kernel/Extensions/IOUSBFamily/include" -I"$d/src/Kernel/Extensions/IOPCIFamily/include" \
      -I"$d/src/Kernel/Extensions/IONetworkingFamily/include" \
      -I"$d/src/Kernel/Extensions/IOHIDFamily/IOHIDSystem" -I"$d/src/Kernel/Extensions/IOHIDFamily/include" \
      -I"$d/src/Kernel/Extensions/IOGraphicsFamily" -I"$d/src/Kernel/Extensions/IOHIDFamily" \
      -I"$d/src/Kernel/Extensions/IOHIDFamily/IOHIDFamily" -fapple-kext -std=gnu++17 -fno-exceptions -fno-rtti -nostdinc++ -c "$c" -o "$o" 2> >(grep -E "error" >&2)
    objs+=("$o")
  done
  local stock="$base/root/Extensions/$name.kext/Contents/Info.plist"
  local id ver
  id=$(plutil -extract CFBundleIdentifier raw "$stock")
  ver=$(plutil -extract CFBundleVersion raw "$stock")
  cat > "$tmp/$name-info.c" <<EOF2
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($id, "$ver", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
EOF2
  clang "${cflags[@]}" -c "$tmp/$name-info.c" -o "$tmp/$name-info.o"
  local kk="$out/$name.kext"
  mkdir -p "$kk/Contents/MacOS"
  cp "$stock" "$kk/Contents/Info.plist"
  clang++ -target x86_64-apple-macos11 -bundle -undefined dynamic_lookup -nostdlib -Wl,-no_fixup_chains \
    -L"$sdk/usr/lib" "${objs[@]}" "$tmp/$name-info.o" -lkmodc++ -lkmod "$base/libSystem.B.dylib" \
    -o "$kk/Contents/MacOS/$name" 2> >(grep -v 'built for newer' >&2)
  if nm -u "$kk/Contents/MacOS/$name" | grep -q chkstk_darwin; then
    echo "$name ruft ___chkstk_darwin (Stapelrahmen >= 4 KB)" >&2; exit 1
  fi
  kexts+=("$kk"); rebuilt+=("$name.kext")
  echo "Treiber: $kk"
}
rebuild_kext PDRealtek8111 PDRealtek8111 "$src/net/pdrealtek8111-rx.patch" '*/src/Kernel/Extensions/PDRealtek8111' \
  '*/src/Kernel/Extensions/IOPCIFamily/include' '*/src/Kernel/Extensions/IONetworkingFamily/include'
rebuild_kext IOUSBHIDDriver IOUSBFamily/IOUSBHIDDriver "$src/usbhid/iousbhid-stall.patch" \
  '*/src/Kernel/Extensions/IOUSBFamily/IOUSBHIDDriver' '*/src/Kernel/Extensions/IOUSBFamily/include' \
  '*/src/Kernel/Extensions/IOPCIFamily/include' '*/src/Kernel/Extensions/IOHIDFamily' \
  '*/src/Kernel/Extensions/IOGraphicsFamily' '*/src/Kernel/Extensions/ApplePS2Controller/ApplePS2KeyboardMap.h'

# --- Kernel Collection --------------------------------------------------------------------------
args=(-kernel "$base/root/Kernels/kernel")
while read -r line; do
  k=$(printf '%s\n' "$line" | sed -n -E 's/.*-kext "\$KEXTS\/([^"]+)".*/\1/p')
  skip=0; for r in "${rebuilt[@]}"; do [ "$k" = "$r" ] && skip=1; done
  [ -n "$k" ] && [ "$skip" = 0 ] && args+=(-kext "$base/root/Extensions/$k")  # die neu gebauten kommen von oben
done < "$base/pd-rebuild-kc"
for k in ${kexts[@]+"${kexts[@]}"}; do args+=(-kext "$k"); done
# Zwei Kernel: kernel mit PureDarwins ACPI-Plattform PDACPIPlatform (uACPI-Interpreter, liest die MADT und startet die
# weiteren Prozessorkerne; läuft auf dem HP mit 12 Threads; Boot-Arg cpus=N löst dort XNUs Topologie-Panik aus, nicht
# benutzen) und kernel-up mit der einfachen AppleI386PlatformExpert (ein Kern). Beide passen auf den Knoten "ACPI";
# in einer Kopie mit höherem IOProbeScore gewinnt die ACPI-Plattform. Der Loader nimmt kernel-up, wenn beim
# Einschalten U gehalten wird (die Taste kommt auf manchen PCs nicht an, z. B. dem HP).
smp="$tmp/PDACPIPlatform.kext"
rm -rf "$smp"
# PDACPIPlatform wird aus PureDarwins Quelle neu gebaut (kext/pdacpi/pdacpi.patch): ohne Änderung stürzt XNU
# auf Hybrid-Prozessoren (Alder Lake) in x86_validate_topology() ab, und nach initialize_screen(6) sieht
# man die Panik nicht. Zusätzlich malt ClokeTrace mit Boot-Arg cloke_trace=1 den Fortschritt auf den Schirm.
fetch_file "$PDSRC_URL" "$PDSRC_SHA256" "$PDSRC_TGZ" >/dev/null
pd="$WORK/tmp/pdacpi-src"
rm -rf "$pd"
mkdir -p "$pd"
tar -xzf "$PDSRC_TGZ" -C "$pd" --strip-components=1 '*/src/Kernel/Extensions/PDACPIPlatform' \
  '*/src/Kernel/Extensions/IOACPIFamily/include' '*/src/Kernel/Extensions/AppleAPIC/PICShared.h'
patch -s -p1 -d "$pd" < "$src/pdacpi/pdacpi.patch"
ext="$pd/src/Kernel/Extensions"
pdir="$ext/PDACPIPlatform"
res=$(clang -print-resource-dir)/include
pflags=("${cflags[@]}" -isystem "$res" -I"$src/pdacpi/include" -I"$src/pdacpi" -I"$pdir/uacpi/include"
        -I"$ext/IOACPIFamily/include" -include "$src/pdacpi/ipi_handler_t.h" -DUACPI_BAREBONES_MODE -Wno-everything)
pobjs=()
for c in "$pdir"/*.cpp "$src/pdacpi/ClokeTrace.cpp"; do
  o="$tmp/pdacpi-$(basename "$c" .cpp).o"
  clang++ "${pflags[@]}" -fapple-kext -std=gnu++17 -fno-exceptions -fno-rtti -nostdinc++ -c "$c" -o "$o"
  pobjs+=("$o")
done
for c in "$pdir"/uacpi/source/*.c; do
  o="$tmp/pdacpi-uacpi-$(basename "$c" .c).o"
  clang "${pflags[@]}" -std=c11 -ffreestanding -c "$c" -o "$o"
  pobjs+=("$o")
done
cat > "$tmp/pdacpi-info.c" <<'EOF'
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(org.puredarwin.driver.PDACPIPlatform, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
EOF
clang "${cflags[@]}" -c "$tmp/pdacpi-info.c" -o "$tmp/pdacpi-info.o"
mkdir -p "$smp/Contents/MacOS"
cp "$pdir/Info.plist" "$smp/Contents/Info.plist"
clang++ -target x86_64-apple-macos11 -bundle -undefined dynamic_lookup -nostdlib -Wl,-no_fixup_chains \
  -L"$sdk/usr/lib" "${pobjs[@]}" "$tmp/pdacpi-info.o" -lkmodc++ -lkmod "$base/libSystem.B.dylib" \
  -o "$smp/Contents/MacOS/PDACPIPlatform" 2> >(grep -v 'built for newer' >&2)
python3 -I - "$smp/Contents/Info.plist" <<'PYEOF2'
import plistlib, sys
p = sys.argv[1]
d = plistlib.load(open(p, 'rb'))
d['IOKitPersonalities']['PDACPI Platform']['IOProbeScore'] = 5000
plistlib.dump(d, open(p, 'wb'), fmt=plistlib.FMT_XML)
PYEOF2
for p in "$base"/root/Extensions/System.kext/PlugIns/*.kext; do args+=(-codeless "$p"); done
runner=()
[ "$(uname -m)" = arm64 ] && runner=(arch -x86_64)
build_kc() { # <ausgabe> [weitere kc-builder-Argumente]
  local o="$1"
  shift
  "${runner[@]}" "$base/kc-builder" "${args[@]}" "$@" -o "$o" > "$tmp/kc-builder.log" 2>&1 ||
    { tail -20 "$tmp/kc-builder.log" >&2; exit 1; }
  for k in ${kexts[@]+"${kexts[@]}"}; do
    id=$(plutil -extract CFBundleIdentifier raw "$k/Contents/Info.plist")
    grep -q "patched kmod_info: $id" "$tmp/kc-builder.log" || { echo "kc-builder hat $id nicht eingebunden" >&2; exit 1; }
  done
  python3 -I "$HERE/kcfix.py" "$o" >/dev/null
}
build_kc "$WORK/out/kernel"
mv "$WORK/out/kernel" "$WORK/out/kernel-up"
build_kc "$WORK/out/kernel" -kext "$smp"
echo "Kernel Collection: $WORK/out/kernel (Mehrkern) und kernel-up (ein Kern) (${#kexts[@]} eigene Treiber)"
