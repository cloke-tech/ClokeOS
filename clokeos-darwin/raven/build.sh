#!/bin/bash
# Baut Raven (https://github.com/mirkobrombin/raven, GPL-3.0) für ClokeOS x86_64 auf dem Mac: Quellen holen, den Darwin-Port
# (raven-darwin.patch: Mach-Backend statt ptrace, Plattform-Shims) anwenden, mit CMake für x86_64-apple-macos11 bauen.
#
#   raven/build.sh <stage>     legt <stage>/usr/libexec/raven/{raven,raven-bootstrap} und die gepatchte libc++ ab
#
# Ohne Vulkan, OpenSSL, Skia, FreeType, libpng und pixman (kein x86_64-Build davon): 64-Bit-Konsolenprogramme und die Teile
# von Raven, die keine Grafik und keine Schriften brauchen (getestet: ein MinGW-Konsolenprogramm mit ucrt-Imports). Nur x86_64: Raven führt x86-64-Code nativ aus.
set -euo pipefail
source "$(dirname "$0")/../config.sh"
need "$DEBUGFS" clang cmake ninja git python3 nm x86_64-w64-mingw32-gcc
[ "$CLOKE_ARCH" = x86_64 ] || { echo "Raven gibt es nur für x86_64" >&2; exit 0; }

stage="$1"
REV=387c718393777983d38a43d269df1ff13bfb5c04
src="$WORK/raven/src"
build="$WORK/raven/build"
mkdir -p "$WORK/raven"
if [ ! -d "$src/.git" ]; then
  git clone -q https://github.com/mirkobrombin/raven.git "$src"
fi
want=$(shasum "$HERE/raven/raven-darwin.patch" | cut -d' ' -f1)
if [ "$(cat "$src/.clokeos-patch" 2>/dev/null)" != "$want" ]; then
  git -C "$src" checkout -q -f "$REV"
  git -C "$src" clean -fdq -- native scripts CMakeLists.txt
  git -C "$src" apply --whitespace=nowarn "$HERE/raven/raven-darwin.patch"
  echo "$want" > "$src/.clokeos-patch"
fi
# die Gast-Komponenten (RTL, ucrtbase, Controls) baut Raven mit MinGW aus den eigenen Quellen
[ -f "$WORK/raven/components/rtl/raven-rtl.dll" ] || python3 "$src/scripts/provision-rtl.py" --output "$WORK/raven/components/rtl" >/dev/null
[ -f "$WORK/raven/components/controls/ucrtbase.dll" ] || python3 "$src/scripts/provision-controls.py" --output "$WORK/raven/components/controls" >/dev/null
cmake -G Ninja -S "$src" -B "$build" -DCMAKE_TOOLCHAIN_FILE="$HERE/raven/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=ON -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
  -DCMAKE_ASM_COMPILER_TARGET=x86_64-apple-macos11.0 -DRAVEN_RTL_COMPONENT_PATH="$WORK/raven/components/rtl/raven-rtl.dll" \
  -DRAVEN_CONTROLS_COMPONENT_DIR="$WORK/raven/components/controls" >/dev/null
ninja -C "$build" raven raven-bootstrap >/dev/null

# PureDarwins libc++ bricht bei std::uncaught_exceptions ab: die Bibliothek wird im Image ersetzt
read -r root_start _ < <(gpt_part "$PD_IMG" "$GUID_LINUX_FS")
fs="$PD_IMG?offset=$((root_start * 512))"
mkdir -p "$WORK/tmp/raven"
"$DEBUGFS" -R "dump /usr/lib/libc++.1.dylib $WORK/tmp/raven/libc++.1.dylib" "$fs" >/dev/null 2>&1
[ -s "$WORK/tmp/raven/libc++.1.dylib" ] || { echo "libc++.1.dylib nicht aus dem Image geholt" >&2; exit 1; }
python3 -I "$src/native/platform/patch_libcxx.py" "$WORK/tmp/raven/libc++.1.dylib" "$WORK/tmp/raven/libc++.1.dylib.patched"

mkdir -p "$stage/usr/libexec/raven" "$stage/usr/lib"
cp "$build/native/raven" "$build/native/raven-bootstrap" "$stage/usr/libexec/raven/"
# die Komponenten liegen neben dem Programm: Raven sucht sie dort
for f in "$build"/native/*.dll "$build"/native/*.exe; do [ -f "$f" ] && cp "$f" "$stage/usr/libexec/raven/"; done
cp "$WORK/tmp/raven/libc++.1.dylib.patched" "$stage/usr/lib/libc++.1.dylib"
chmod 755 "$stage/usr/libexec/raven/raven" "$stage/usr/libexec/raven/raven-bootstrap" "$stage/usr/lib/libc++.1.dylib"
echo "Fertig: Raven für x86_64 ($(wc -c < "$build/native/raven" | tr -d ' ') Bytes)"
