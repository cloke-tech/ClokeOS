#!/bin/bash
# Baut ClokeOS' eigene Xorg-Treiber (xdriver/<name>/<name>_drv.c) auf dem Mac für PureDarwin (x86_64).
#
#   xdriver/build.sh [ausgabeordner]     Standard: ~/ClokeOS-work/out/xdriver; Ergebnis <name>_drv.so,
#                                        im Image unter /usr/lib/xorg/modules/drivers/
#
# Header des Xorg-SDK (/usr/include/xorg, /include/X11) kommen mit debugfs aus PureDarwins Desktop-Image,
# pixman.h fehlt dort und kommt von Homebrew (pixman, MIT; nur Typen). Gelinkt wird wie PureDarwins
# puredarwingop: als dylib (PureDarwins dyld lädt kein MH_BUNDLE), flacher Namensraum, die Symbole des
# X-Servers bleiben offen und bindet dyld beim Laden gegen Xorg; nur <name>ModuleData wird exportiert.
# Dazu libSystem, IOKit und CoreFoundation aus dem Image (wie tools/build.sh).
set -euo pipefail
export CLOKEOS_FLAVOR=desktop
CLOKEOS_ARCH=x86_64
export CLOKEOS_ARCH
source "$(dirname "$0")/../config.sh"
need "$DEBUGFS" clang
out="${1:-$WORK/out/xdriver}"
sdk=$(xcrun --show-sdk-path)
read -r root_start _ < <(gpt_part "$PD_IMG" "$GUID_LINUX_FS")
fs="$PD_IMG?offset=$((root_start * 512))"

x="$WORK/sysroot/xsdk-${PD_SHA256:0:12}"
if [ ! -f "$x/.fertig" ]; then
  rm -rf "$x"
  mkdir -p "$x/include" "$x/lib"
  "$DEBUGFS" -R "rdump /usr/include/xorg $x/include" "$fs" >/dev/null 2>&1
  "$DEBUGFS" -R "rdump /include/X11 $x/include" "$fs" >/dev/null 2>&1
  for f in /usr/lib/libSystem.B.dylib /usr/lib/libIOKitCF.dylib /usr/lib/libCoreFoundation.dylib; do
    "$DEBUGFS" -R "dump $f $x/lib/$(basename "$f")" "$fs" >/dev/null 2>&1
  done
  for f in include/xorg/xf86.h include/X11/X.h lib/libSystem.B.dylib lib/libIOKitCF.dylib; do
    [ -s "$x/$f" ] || { echo "Nicht aus dem Image geholt: $f" >&2; exit 1; }
  done
  touch "$x/.fertig"
fi
pixman=$(brew --prefix pixman 2>/dev/null)/include/pixman-1
[ -f "$pixman/pixman.h" ] || { echo "pixman.h fehlt (brew install pixman)" >&2; exit 1; }

mkdir -p "$out"
tmp="$WORK/tmp/xdriver"
mkdir -p "$tmp"
for dir in "$HERE"/xdriver/*/; do
  name=$(basename "$dir")
  src="$dir/${name}_drv.c"
  [ -f "$src" ] || continue
  echo "_${name}ModuleData" > "$tmp/$name.exports"
  clang -target x86_64-apple-macos11 -isysroot "$sdk" -O2 -Wall -Wno-unused-parameter -fno-stack-protector \
    -DIN_MODULE=1 -I"$x/include/xorg" -I"$x/include" -I"$pixman" -I"$HERE/kext/ClokeIntelGfx" \
    -ffile-prefix-map="$HERE"=clokeos-darwin -c "$src" -o "$tmp/$name.o"
  clang -target x86_64-apple-macos11 -dynamiclib -nostdlib -flat_namespace -Wl,-undefined,suppress -Wl,-not_for_dyld_shared_cache \
    -Wl,-exported_symbols_list,"$tmp/$name.exports" -Wl,-platform_version,macos,11.0,11.5 -Wl,-no_fixup_chains \
    -install_name "/usr/lib/xorg/modules/drivers/${name}_drv.so" \
    "$tmp/$name.o" "$x/lib/libSystem.B.dylib" "$x/lib/libIOKitCF.dylib" "$x/lib/libCoreFoundation.dylib" \
    -o "$out/${name}_drv.so" 2> >(grep -v -E 'built for newer|-undefined suppress is deprecated' >&2)
  echo "Xorg-Treiber: $out/${name}_drv.so"
done
