#!/bin/bash
# Baut clokesearch (Search auf WebKitGTK) auf dem Mac für PureDarwin arm64.
#   webkit/search/build.sh <webkit-builddir> <webkit-quellbaum> <ausgabe>
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/../../config.sh"
b="$1"; src="$2"; out="$3"; sr="$WORK/sysroot/webkit-$CLOKE_ARCH"; sdk="$(xcrun --show-sdk-path)"
export PKG_CONFIG_LIBDIR="$sr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$sr"
# jsc/-Header liegen teils im Quellbaum, teils abgeleitet: in einem Ordner zusammenlegen
h="$b/clokeos-hdr"; mkdir -p "$h/jsc"
for f in "$src"/Source/JavaScriptCore/API/glib/*.h "$b"/JavaScriptCoreGLib/DerivedSources/jsc/*.h; do ln -sf "$f" "$h/jsc/$(basename "$f")"; done
cflags="$(pkg-config --cflags gtk+-3.0 libsoup-3.0)"
# shellcheck disable=SC2086
clang -target ${CLOKE_ARCH}-apple-macos11.0 -isysroot "$sdk" -isystem "$sr/include" $cflags \
  -I"$b/WebKitGTK/DerivedSources" -I"$h" -DWEBKIT2_COMPILATION=0 \
  -O2 -nostdlib -L"$sr/usr/lib" -L"$sr/lib" -L"$b/lib" -Wl,-platform_version,macos,11.0,11.5 -Wl,-no_fixup_chains \
  -o "$out" "$here/clokesearch.c" -lSystem -lwebkit2gtk-4.1 -ljavascriptcoregtk-4.1 -lgtk-3 -lgdk-3 -lgobject-2.0 -lglib-2.0 -lgio-2.0
install_name_tool -change lib/libwebkit2gtk-4.1.dylib /opt/webkit/lib/libwebkit2gtk-4.1.dylib "$out"
install_name_tool -change lib/libjavascriptcoregtk-4.1.dylib /opt/webkit/lib/libjavascriptcoregtk-4.1.dylib "$out" 2>/dev/null || true
codesign -s - -f "$out"
