#!/bin/bash
# Baut Raven (https://github.com/mirkobrombin/raven, GPL-3.0) für ClokeOS x86_64 auf dem Mac: Quellen holen, den Darwin-Port
# (raven-darwin.patch: Mach-Backend statt ptrace, Plattform-Shims) anwenden, mit CMake für x86_64-apple-macos11 bauen.
#
#   raven/build.sh <stage>     legt <stage>/usr/libexec/raven/{raven,raven-bootstrap} und die gepatchte libc++ ab
#
# Ohne Vulkan, OpenSSL, Skia, FreeType und libpng: 64-Bit-Konsolenprogramme und einfache Fenster über den X11-Desktop (xcb,
# xkbcommon, pixman); mit Skia (nur CPU) und Liberation-Schriften auch Text. Getestet: ein Konsolenprogramm, ein GDI-Fenster mit Text. Nur x86_64: Raven führt x86-64-Code nativ aus.
set -euo pipefail
source "$(dirname "$0")/../config.sh"
need "$DEBUGFS" clang cmake ninja git python3 nm curl brew x86_64-w64-mingw32-gcc pkg-config
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
# Sysroot für den X11-Desktop: Header und Bibliotheken (xcb, xkbcommon, freetype, png) kommen aus dem PureDarwin-Image, pixman
# wird aus dem Quelltext gebaut (meson aus einer venv)
sys="$WORK/raven/sysroot"
if [ ! -f "$sys/lib/libpixman-1.a" ]; then
  rm -rf "$sys"; mkdir -p "$sys/lib/pkgconfig"
  read -r root_start _ < <(gpt_part "$PD_IMG" "$GUID_LINUX_FS")
  fs0="$PD_IMG?offset=$((root_start * 512))"
  "$DEBUGFS" -R "rdump /include $sys" "$fs0" >/dev/null 2>&1
  for f in $("$DEBUGFS" -R "ls -l /lib" "$fs0" 2>/dev/null | awk '{print $NF}' | grep -E "^lib(xcb|xkbcommon|freetype|png|X11|Xau|Xdmcp|fontconfig|expat)[^/]*\.(a|dylib)$"); do
    "$DEBUGFS" -R "dump /lib/$f $sys/lib/$f" "$fs0" >/dev/null 2>&1
  done
  for f in xcb xcb-xkb xcb-xfixes xcb-xinput xkbcommon xkbcommon-x11 freetype2 xproto xau xdmcp pthread-stubs; do
    "$DEBUGFS" -R "dump /lib/pkgconfig/$f.pc $sys/lib/pkgconfig/$f.pc" "$fs0" >/dev/null 2>&1
  done
  # Verknüpfungen kommen aus dem Image als leere Dateien: durch die versionierte Bibliothek ersetzen
  for f in "$sys"/lib/*.dylib; do
    [ -s "$f" ] && continue
    b=${f%.dylib}; t=$(ls -S "$b".*.dylib 2>/dev/null | head -1)
    [ -n "$t" ] && [ -s "$t" ] && cp "$t" "$f"
  done
  for f in "$sys"/lib/pkgconfig/*.pc; do
    [ -f "$f" ] || continue
    sed -i '' -e "s#^prefix=.*#prefix=$sys#" -e '/^Requires.private/d' -e '/^Libs.private/d' -e "s#/nix/store/[a-z0-9]*-[A-Za-z0-9._-]*#$sys#g" "$f"
  done
  # OpenSSL (Zertifikate in crypt32): Bibliotheken aus dem Image, Header von Homebrews openssl@3 (gleiche Hauptversion)
  mkdir -p "$sys/ssl/include" "$WORK/raven/dl"
  cp -R "$(brew --prefix openssl@3)/include/openssl" "$sys/ssl/include/"
  for f in libcrypto.a libssl.a; do "$DEBUGFS" -R "dump /lib/$f $sys/lib/$f" "$fs0" >/dev/null 2>&1; done
  # libpng 1.6.59 (Raven verlangt mindestens diese Version, das Image hat 1.6.58)
  png=1.6.59
  [ -d "$WORK/raven/dl/libpng-$png" ] || { curl -sL "https://download.sourceforge.net/libpng/libpng-$png.tar.xz" | tar xJ -C "$WORK/raven/dl"; }
  cmake -G Ninja -S "$WORK/raven/dl/libpng-$png" -B "$WORK/raven/dl/libpng-build" -DCMAKE_TOOLCHAIN_FILE="$HERE/raven/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$sys/png" >/dev/null
  ninja -C "$WORK/raven/dl/libpng-build" install >/dev/null
  pixman=0.46.4
  mkdir -p "$WORK/raven/dl"
  [ -d "$WORK/raven/dl/pixman-$pixman" ] || { curl -sL "https://www.cairographics.org/releases/pixman-$pixman.tar.gz" | tar xz -C "$WORK/raven/dl"; }
  python3 -m venv "$WORK/raven/dl/mv" >/dev/null && "$WORK/raven/dl/mv/bin/pip" -q install meson >/dev/null 2>&1
  cat > "$WORK/raven/dl/cross.ini" <<INI
[binaries]
c = ['clang', '-target', 'x86_64-apple-macos11.0']
cpp = ['clang++', '-target', 'x86_64-apple-macos11.0']
ar = 'ar'
strip = 'strip'
[host_machine]
system = 'darwin'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[built-in options]
c_args = ['-fPIC']
INI
  PATH="$WORK/raven/dl/mv/bin:$PATH" meson setup "$WORK/raven/dl/pixman-build" "$WORK/raven/dl/pixman-$pixman" --cross-file "$WORK/raven/dl/cross.ini" \
    --default-library=static -Dopenmp=disabled -Dgtk=disabled -Dlibpng=disabled -Dtests=disabled -Ddemos=disabled -Dprefix="$sys" -Dbuildtype=release >/dev/null
  PATH="$WORK/raven/dl/mv/bin:$PATH" ninja -C "$WORK/raven/dl/pixman-build" install >/dev/null
fi
export PKG_CONFIG_LIBDIR="$sys/lib/pkgconfig" PKG_CONFIG_PATH="$sys/lib/pkgconfig"
# Text in Fenstern: Skia (nur CPU, ohne CoreText) und die Schriften. Von Skia wird nur FreeType gebraucht; git-sync-deps holt
# dagegen 7 GB, deshalb wird FreeType allein geholt.
text="$WORK/raven/text"
mkdir -p "$text/pnginc"
cp "$sys/include/png.h" "$sys/include/pngconf.h" "$sys/include/pnglibconf.h" "$text/pnginc/"
if [ ! -f "$text/cpu-fonts/libskia.a" ]; then
  skia_rev=a21aae83d6e4e3da284100a30f0a5eafc443b60f
  if [ ! -d "$text/skia/.git" ]; then
    git init -q "$text/skia"
    git -C "$text/skia" fetch -q --depth=1 https://skia.googlesource.com/skia.git "$skia_rev"
    git -C "$text/skia" checkout -q FETCH_HEAD
  fi
  if [ ! -f "$text/skia/third_party/externals/freetype/include/freetype/freetype.h" ]; then
    ft_rev=$(sed -n 's#.*"third_party/externals/freetype" *: *"[^"]*@\([0-9a-f]*\)".*#\1#p' "$text/skia/DEPS")
    mkdir -p "$text/skia/third_party/externals/freetype"
    git -C "$text/skia/third_party/externals/freetype" init -q
    git -C "$text/skia/third_party/externals/freetype" fetch -q --depth=1 https://chromium.googlesource.com/chromium/src/third_party/freetype2.git "$ft_rev"
    git -C "$text/skia/third_party/externals/freetype" checkout -q FETCH_HEAD
  fi
  python3 "$text/skia/bin/fetch-gn" >/dev/null
  mkdir -p "$text/cpu-fonts"
  python3 - "$src/scripts/provision-text.py" "$text" <<'PY'
import re, sys
args = re.search(r'SKIA_ARGUMENTS = """(.*?)"""', open(sys.argv[1]).read(), re.S).group(1)
args += 'target_os = "mac"\ntarget_cpu = "x64"\nskia_use_system_freetype2 = false\nskia_use_fonthost_mac = false\n'
args += 'extra_cflags = ["-mmacosx-version-min=11.0", "-fPIC", "-isystem", "%s/pnginc"]\n' % sys.argv[2]
open(sys.argv[2] + "/cpu-fonts/args.gn", "w").write(args)
PY
  (cd "$text/skia" && ./bin/gn gen "$text/cpu-fonts" >/dev/null)
  ninja -C "$text/cpu-fonts" skia >/dev/null
fi
if [ ! -f "$text/fonts/RavenSans-Regular.ttf" ]; then
  # macOS-tar kennt --wildcards nicht; fontTools aus einer venv
  mkdir -p "$text/shim"
  printf '#!/bin/bash\nargs=(); for a in "$@"; do [ "$a" = --wildcards ] || args+=("$a"); done\nexec /usr/bin/tar "${args[@]}"\n' > "$text/shim/tar"
  chmod +x "$text/shim/tar"
  [ -x "$WORK/raven/dl/mv/bin/pip" ] || python3 -m venv "$WORK/raven/dl/mv" >/dev/null
  "$WORK/raven/dl/mv/bin/pip" -q install fonttools >/dev/null 2>&1
  PATH="$text/shim:$PATH" "$WORK/raven/dl/mv/bin/python" "$src/scripts/provision-text.py" --output "$text" --fonts-only >/dev/null
fi
# die Gast-Komponenten (RTL, ucrtbase, Controls) baut Raven mit MinGW aus den eigenen Quellen
[ -f "$WORK/raven/components/rtl/raven-rtl.dll" ] || python3 "$src/scripts/provision-rtl.py" --output "$WORK/raven/components/rtl" >/dev/null
[ -f "$WORK/raven/components/controls/ucrtbase.dll" ] || python3 "$src/scripts/provision-controls.py" --output "$WORK/raven/components/controls" >/dev/null
cmake -G Ninja -S "$src" -B "$build" -DCMAKE_TOOLCHAIN_FILE="$HERE/raven/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON \
  -DFREETYPE_INCLUDE_DIR_freetype2="$sys/include/freetype2" -DFREETYPE_INCLUDE_DIR_ft2build="$sys/include/freetype2" \
  -DFREETYPE_LIBRARY_RELEASE="$sys/lib/libfreetype.dylib" -DRAVEN_SKIA_SOURCE="$text/skia" -DRAVEN_SKIA_LIBRARY="$text/cpu-fonts/libskia.a" \
  -DRAVEN_FONT_DIRECTORY="$text/fonts" -DOPENSSL_INCLUDE_DIR="$sys/ssl/include" -DOPENSSL_CRYPTO_LIBRARY="$sys/lib/libcrypto.a" -DOPENSSL_SSL_LIBRARY="$sys/lib/libssl.a" \
  -DPNG_LIBRARY_RELEASE="$sys/png/lib/libpng16.a" -DPNG_PNG_INCLUDE_DIR="$sys/png/include" -DRAVEN_DESKTOP_WAYLAND=OFF -DCMAKE_FIND_ROOT_PATH="$sys" \
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
cp -R "$text/fonts" "$stage/usr/libexec/raven/fonts"
# die Komponenten liegen neben dem Programm: Raven sucht sie dort
for f in "$build"/native/*.dll "$build"/native/*.exe; do [ -f "$f" ] && cp "$f" "$stage/usr/libexec/raven/"; done
cp "$WORK/tmp/raven/libc++.1.dylib.patched" "$stage/usr/lib/libc++.1.dylib"
chmod 755 "$stage/usr/libexec/raven/raven" "$stage/usr/libexec/raven/raven-bootstrap" "$stage/usr/lib/libc++.1.dylib"
echo "Fertig: Raven für x86_64 ($(wc -c < "$build/native/raven" | tr -d ' ') Bytes)"
