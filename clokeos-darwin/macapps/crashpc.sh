#!/bin/bash
# Baut libcrashpc.dylib (macapps/crashpc.c, arm64) nach <stage>/usr/lib – Aufruf aus brand.sh über CLOKE_EXTRA_STAGE oder von Hand:
#   macapps/crashpc.sh <ordner>
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$1/usr/lib"
clang -arch arm64 -mmacosx-version-min=11.0 -dynamiclib -O1 -install_name /usr/lib/libcrashpc.dylib -lobjc -o "$1/usr/lib/libcrashpc.dylib" "$here/crashpc.c" 2>&1 | grep -v warning || true
echo "$1/usr/lib/libcrashpc.dylib"
# libclogictrace: Startkette von Logic Pro (macapps/logictrace.c)
clang -arch arm64 -mmacosx-version-min=11.0 -dynamiclib -O1 -install_name /usr/lib/libclogictrace.dylib -lobjc -Wl,-not_for_dyld_shared_cache -Wl,-undefined,dynamic_lookup -o "$1/usr/lib/libclogictrace.dylib" "$here/logictrace.c" 2>&1 | grep -v warning || true
echo "$1/usr/lib/libclogictrace.dylib"
