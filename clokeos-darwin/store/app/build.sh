#!/bin/bash
# Baut clokeapp (WebKit-Fenster ohne Adressleiste) auf dem Mac für PureDarwin, beide Architekturen, als Universal-Binary.
#   store/app/build.sh <ausgabe> [<ordner-mit-arm64/opt/webkit/lib und x86_64/opt/webkit/lib>]
# Die WebKit-Bibliothek kommt aus dem Paket webkit (cloke-tech/ClokeOS, Pre-Release webkit-<arch>-1); der GTK-Teil aus dem Sysroot
# von webkit/sysroot.sh. Ohne Sysroot einer Architektur wird nur die andere gebaut.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/../../config.sh"
out="$1"; libs="${2:-$WORK/webkit-libs}"; sdk="$(xcrun --show-sdk-path)"; tmp="$(mktemp -d)"; built=()
for arch in arm64 x86_64; do
  sr="$WORK/sysroot/webkit-$arch"; l="$libs/$arch/opt/webkit/lib"
  [ -d "$sr" ] && [ -f "$l/libwebkit2gtk-4.1.dylib" ] || { echo "ohne $arch (Sysroot oder Bibliothek fehlt)"; continue; }
  export PKG_CONFIG_LIBDIR="$sr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$sr"
  # shellcheck disable=SC2046
  clang -target ${arch}-apple-macos11.0 -isysroot "$sdk" -isystem "$sr/include" $(pkg-config --cflags gtk+-3.0) \
    -O2 -nostdlib -L"$sr/usr/lib" -L"$sr/lib" -L"$l" -Wl,-platform_version,macos,11.0,11.5 -Wl,-no_fixup_chains \
    -o "$tmp/clokeapp-$arch" "$here/clokeapp.c" -lSystem -lwebkit2gtk-4.1 -lgtk-3 -lgdk-3 -lgobject-2.0 -lglib-2.0 -lgio-2.0
  install_name_tool -change lib/libwebkit2gtk-4.1.dylib /opt/webkit/lib/libwebkit2gtk-4.1.dylib "$tmp/clokeapp-$arch"
  built+=("$tmp/clokeapp-$arch")
done
[ ${#built[@]} -gt 0 ] || { echo "nichts gebaut" >&2; exit 1; }
lipo -create "${built[@]}" -output "$out"
codesign -s - -f "$out"
rm -rf "$tmp"
