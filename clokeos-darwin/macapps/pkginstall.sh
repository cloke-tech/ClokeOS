#!/bin/bash
# Baut clokeos-pkginstall (macapps/pkginstall.c) für ClokeOS.
#
#   macapps/pkginstall.sh                        x86_64, nach ~/ClokeOS-work/out/macapps/root/usr/sbin/
#   CLOKEOS_ARCH=arm64 macapps/pkginstall.sh     arm64, nach ~/ClokeOS-work/out/macapps-arm64/root/usr/sbin/
#   macapps/pkginstall.sh host <ziel>            für den Mac, auf dem gebaut wird (Tests)
#
# Braucht nur libSystem und libz (beide in PureDarwin). Ohne foundation.sh-Ausgabe (pdlibs) baut es
# gegen Apples SDK und setzt nur die Zielversion.
set -euo pipefail
source "$(dirname "$0")/../config.sh"
M="$HERE/macapps"
SDK="$(xcrun --show-sdk-path)"
if [ "${1:-}" = host ]; then
  clang -O2 -Wall -Wno-parentheses -isysroot "$SDK" -o "$2" "$M/pkginstall.c" -lz
  echo "Fertig: $2"; exit 0
fi
out="$WORK/out/macapps"; extra=()
[ "$CLOKE_ARCH" = arm64 ] && { out="$WORK/out/macapps-arm64"; extra=(-fno-stack-check -Wl,-no_fixup_chains); }
pd="$out/pdlibs"
[ -f "$pd/usr/lib/libz.1.dylib" ] || { echo "Fehlt: $pd/usr/lib/libz.1.dylib (zuerst macapps/foundation.sh)" >&2; exit 1; }
mkdir -p "$out/root/usr/sbin"
clang -target "$CLOKE_ARCH-apple-macos11.0" -isysroot "$SDK" -O2 -Wall -Wno-parentheses ${extra[@]+"${extra[@]}"} \
  -nostdlib -Wl,-platform_version,macos,11.0,11.5 -Wl,-syslibroot,"$pd" \
  -o "$out/root/usr/sbin/clokeos-pkginstall" "$M/pkginstall.c" "$pd/usr/lib/libSystem.B.dylib" "$pd/usr/lib/libz.1.dylib" -I"$SDK/usr/include" 2>&1 | grep -v '^ld: warning' || true
echo "Fertig: $out/root/usr/sbin/clokeos-pkginstall"
