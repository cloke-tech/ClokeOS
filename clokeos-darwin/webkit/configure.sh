#!/bin/bash
# Konfiguriert den WebKit-Cross-Build für PureDarwin arm64.
#   webkit/configure.sh <port: JSCOnly|GTK> <quellbaum> <builddir> [weitere cmake-Optionen]
# Voraussetzung: webkit/sysroot.sh gelaufen, cmake, ninja, perl, ruby, gperf, python3 auf dem Mac.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/../config.sh"
export WEBKIT_ARCH="$CLOKE_ARCH"
export WEBKIT_SYSROOT="$WORK/sysroot/webkit-$CLOKE_ARCH"
port="$1"; src="$2"; bld="$3"; shift 3
export WEBKIT_SRC="$src"
u="$WEBKIT_SYSROOT/usr/lib"
sdk="$(xcrun --show-sdk-path)"
clang -target ${CLOKE_ARCH}-apple-macos11.0 -isysroot "$sdk" -O2 -fno-builtin -c "$here/compat/pdcompat.c" -o /tmp/pdcompat.$$.o
clang -target ${CLOKE_ARCH}-apple-macos11.0 -isysroot "$sdk" -c "$here/../libcompat/chkstk.S" -o /tmp/pdchkstk.$$.o
rm -f "$u/libpdcompat.a"; ar rcs "$u/libpdcompat.a" /tmp/pdcompat.$$.o /tmp/pdchkstk.$$.o; rm -f /tmp/pdcompat.$$.o /tmp/pdchkstk.$$.o
rm -rf "$bld"; mkdir -p "$bld"; cd "$bld"
cmake -G Ninja -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" -DCMAKE_TOOLCHAIN_FILE="$here/toolchain-pd.cmake" -DPORT="$port" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_SYSROOT="$sdk" -DDEVELOPER_MODE=OFF -DUSE_HEADER_MAPS=OFF -DPYTHON_EXECUTABLE="$(command -v python3)" \
  -DICU_UC_LIBRARY_RELEASE="$u/libicuuc.dylib" -DICU_I18N_LIBRARY_RELEASE="$u/libicui18n.dylib" \
  -DICU_DATA_LIBRARY_RELEASE="$u/libicudata.dylib" -DICU_INCLUDE_DIR="$WEBKIT_SYSROOT/include" "$@" "$src"
