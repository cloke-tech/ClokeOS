#!/bin/bash
# Baut die kleinen C-Bibliotheken, die Mac-Programme (Logic) aus /usr/lib erwarten und die das
# PureDarwin-Abbild nicht hat, als dylibs mit Apples Installnamen und Versionen:
#
#   /usr/lib/libsqlite3.dylib    (kompatibel 9.0.0, aktuell 377.0.0)   SQLite 3.46.1
#   /usr/lib/libxml2.2.dylib     (10.0.0, 10.9.0)                      libxml2 2.9.14, ohne iconv/lzma/ICU
#   /usr/lib/libbz2.1.0.dylib    (1.0.0, 1.0.8)                        bzip2 1.0.8
#   /usr/lib/libpcre.0.dylib     (1.0.0, 1.1.0)                        PCRE 8.45 (UTF-8, Unicode-Eigenschaften, kein JIT)
#   /usr/lib/libxar.1.dylib      (1.0.0, 1.3.0)                        xar 1.6.1 (mackyle), Digests über libs/xar-shim statt OpenSSL
#
#   macapps/libs.sh                         x86_64, nach ~/ClokeOS-work/out/macapps/root/usr/lib/
#   CLOKEOS_ARCH=arm64 macapps/libs.sh      arm64, nach ~/ClokeOS-work/out/macapps-arm64/root/usr/lib/
#   macapps/libs.sh host                    für den Mac selbst, nach ~/ClokeOS-work/out/libs-host/ (Installnamen dort)
#                                           und führt libstest aus
#
# Gebaut wird mit Apples clang gegen PureDarwins libSystem und libz (wie accelerate.sh). Die
# Quellen kommen mit Prüfsumme nach ~/ClokeOS-work/download/libs/ (zusammen 9 MB). Außerdem
# entsteht root/Library/ClokeOS/Probe/libstest (ruft je eine Funktion jeder Bibliothek auf).
set -euo pipefail
source "$(dirname "$0")/../config.sh"
M="$HERE/macapps"
SDK="$(xcrun --show-sdk-path)"
mode=target; [ "${1:-}" = host ] && mode=host

D="$WORK/download/libs"
fetch_file https://www.sqlite.org/2024/sqlite-autoconf-3460100.tar.gz 67d3fe6d268e6eaddcae3727fce58fcc8e9c53869bdd07a0c61e38ddf2965071 "$D/sqlite-autoconf-3460100.tar.gz"
fetch_file https://download.gnome.org/sources/libxml2/2.9/libxml2-2.9.14.tar.xz 60d74a257d1ccec0475e749cba2f21559e48139efba6ff28224357c7c798dfee "$D/libxml2-2.9.14.tar.xz"
fetch_file https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269 "$D/bzip2-1.0.8.tar.gz"
fetch_file https://ftp.exim.org/pub/pcre/pcre-8.45.tar.bz2 4dae6fdcd2bb0bb6c37b5f97c33c2be954da743985369cddac3546e3218bffb8 "$D/pcre-8.45.tar.bz2"
fetch_file https://github.com/mackyle/xar/archive/refs/tags/xar-1.6.1.tar.gz 5e7d50dab73f5cb1713b49fa67c455c2a0dd2b0a7770cbc81b675e21f6210e25 "$D/xar-1.6.1.tar.gz"

if [ $mode = host ]; then
  arch="$(uname -m)"; out="$WORK/out/libs-host"; root="$out"; pfx="$out/usr/lib"
  cc=(clang -target "$arch-apple-macos11.0" -isysroot "$SDK")
  ld=(); libsys=(); libz=(-lz)
else
  arch="$CLOKE_ARCH"; out="$WORK/out/macapps"; extra=()
  [ "$arch" = arm64 ] && { out="$WORK/out/macapps-arm64"; extra=(-fno-stack-check); }
  root="$out/root"; pd="$out/pdlibs"; pfx=/usr/lib
  [ -f "$pd/usr/lib/libz.1.dylib" ] || { echo "Fehlt: $pd/usr/lib/libz.1.dylib (zuerst macapps/foundation.sh)" >&2; exit 1; }
  cc=(clang -target "$arch-apple-macos11.0" -isysroot "$SDK" ${extra[@]+"${extra[@]}"})
  ld=(-nostdlib -Wl,-platform_version,macos,11.0,11.5 -Wl,-syslibroot,"$pd")
  [ "$arch" = arm64 ] && ld+=(-Wl,-no_fixup_chains)
  libsys=("$pd/usr/lib/libSystem.B.dylib"); libz=("$pd/usr/lib/libz.1.dylib")
fi
lib="$root/usr/lib"; inc="$out/libs-include"; src="$WORK/tmp/libs-src-$mode-$arch"
rm -rf "$src" "$lib/libsqlite3.dylib" "$lib/libxml2.2.dylib" "$lib/libbz2.1.0.dylib" "$lib/libpcre.0.dylib" "$lib/libxar.1.dylib"
mkdir -p "$src" "$lib" "$inc"
cd "$src"
for a in sqlite-autoconf-3460100.tar.gz libxml2-2.9.14.tar.xz bzip2-1.0.8.tar.gz pcre-8.45.tar.bz2 xar-1.6.1.tar.gz; do tar xf "$D/$a"; done
O="$src/obj"; mkdir -p "$O"

# dylib <name> <installname> <kompatibel> <aktuell> <Quellen…> [-- <Linkargumente…>] ; CFLAGS aus dem Aufrufer
dylib() {
  local name="$1" inst="$2" compat="$3" cur="$4"; shift 4
  local srcs=() lflags=()
  while [ $# -gt 0 ] && [ "$1" != -- ]; do srcs+=("$1"); shift; done
  [ "${1:-}" = -- ] && shift; lflags=("$@")
  local objs=() i=0
  mkdir -p "$O/$name"
  for s in "${srcs[@]}"; do
    local o="$O/$name/$i.o"; i=$((i+1))
    "${cc[@]}" -O2 -g0 -w -fPIC "${CF[@]}" -c "$s" -o "$o" &
    objs+=("$o")
    [ $((i % 8)) -eq 0 ] && wait
  done
  wait
  for o in "${objs[@]}"; do [ -f "$o" ] || { echo "Übersetzen von $name fehlgeschlagen" >&2; exit 1; }; done
  "${cc[@]}" -dynamiclib ${ld[@]+"${ld[@]}"} -install_name "$inst" -compatibility_version "$compat" -current_version "$cur" \
    -o "$lib/$(basename "$inst")" "${objs[@]}" ${lflags[@]+"${lflags[@]}"} ${libsys[@]+"${libsys[@]}"} 2>&1 | grep -v '^ld: warning' || true
  [ -f "$lib/$(basename "$inst")" ] || { echo "Linken von $name fehlgeschlagen" >&2; exit 1; }
  echo "  $(basename "$inst")  $(stat -f %z "$lib/$(basename "$inst")") Bytes"
}

echo "sqlite3"
CF=(-DSQLITE_THREADSAFE=2 -DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_FTS3_PARENTHESIS -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE
    -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_ENABLE_DBSTAT_VTAB -DSQLITE_ENABLE_UNLOCK_NOTIFY -DSQLITE_ENABLE_STAT4 -DSQLITE_ENABLE_PREUPDATE_HOOK
    -DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_API_ARMOR -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_USE_URI=1 -DSQLITE_DQS=0)
dylib sqlite3 "$pfx/libsqlite3.dylib" 9.0.0 377.0.0 "$src/sqlite-autoconf-3460100/sqlite3.c"
cp "$src/sqlite-autoconf-3460100/sqlite3.h" "$src/sqlite-autoconf-3460100/sqlite3ext.h" "$inc/"

echo "libbz2"
b="$src/bzip2-1.0.8"; CF=(-D_FILE_OFFSET_BITS=64)
dylib bz2 "$pfx/libbz2.1.0.dylib" 1.0.0 1.0.8 "$b"/{blocksort,huffman,crctable,randtable,compress,decompress,bzlib}.c
cp "$b/bzlib.h" "$inc/"

echo "libxml2"
# config.h und xmlversion.h erzeugt configure auf dem Mac (gleiche Typgrößen für beide 64-Bit-Architekturen)
x="$src/libxml2-2.9.14"
( cd "$x" && ./configure --without-python --without-lzma --without-iconv --without-icu --with-zlib --disable-static >/dev/null 2>&1 ) || { echo "libxml2: configure fehlgeschlagen" >&2; exit 1; }
CF=(-DHAVE_CONFIG_H -I"$x" -I"$x/include" -D_REENTRANT)
dylib xml2 "$pfx/libxml2.2.dylib" 10.0.0 10.9.0 "$x"/{SAX,entities,encoding,error,parserInternals,parser,tree,hash,list,xmlIO,xmlmemory,uri,valid,xlink,HTMLparser,HTMLtree,debugXML,xpath,xpointer,xinclude,nanohttp,nanoftp,catalog,globals,threads,c14n,xmlstring,buf,xmlregexp,xmlschemas,xmlschemastypes,xmlunicode,xmlreader,relaxng,dict,SAX2,legacy,chvalid,pattern,xmlsave,xmlmodule,xmlwriter,schematron,DOCBparser}.c -- "${libz[@]}"
rm -rf "$inc/libxml"; cp -R "$x/include/libxml" "$inc/libxml"

echo "libpcre"
p="$src/pcre-8.45"
( cd "$p" && ./configure --enable-utf8 --enable-unicode-properties --disable-jit --disable-cpp --disable-shared >/dev/null 2>&1 && cp pcre_chartables.c.dist pcre_chartables.c ) || { echo "pcre: configure fehlgeschlagen" >&2; exit 1; }
CF=(-DHAVE_CONFIG_H -I"$p")
dylib pcre "$pfx/libpcre.0.dylib" 1.0.0 1.1.0 "$p"/{pcre_byte_order,pcre_compile,pcre_config,pcre_dfa_exec,pcre_exec,pcre_fullinfo,pcre_get,pcre_globals,pcre_jit_compile,pcre_maketables,pcre_newline,pcre_ord2utf8,pcre_refcount,pcre_string_utils,pcre_study,pcre_tables,pcre_ucd,pcre_valid_utf8,pcre_version,pcre_xclass,pcre_chartables}.c
cp "$p/pcre.h" "$inc/"

echo "libxar"
r="$src/xar-xar-1.6.1/xar"; mkdir -p "$src/xarinc/xar"
sed -e 's/@XAR_VERSION@/1.6/' -e 's/@XAR_NUMERIC_VERSION@/0x01060180/' -e 's/@C99_FIXED_WIDTH_TYPE_HEADER@/#include <stdint.h>/' "$r/include/xar.h.in" > "$src/xarinc/xar/xar.h"
cp "$src/xarinc/xar/xar.h" "$src/xarinc/xar.h"
cat > "$src/xarinc/config.h" <<'CFG'
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_XATTR_H 1
#define HAVE_GETXATTR 1
#define HAVE_SETXATTR 1
#define HAVE_GETATTRLIST 1
#define HAVE_SETATTRLIST 1
#define HAVE_CHFLAGS 1
#define HAVE_STATFS 1
#define HAVE_STATVFS 1
#define HAVE_STRUCT_STAT_ST_FLAGS 1
#define HAVE_STRUCT_STATFS_F_FSTYPENAME 1
#define HAVE_ASPRINTF 1
#define HAVE_LIBBZ2 1
#define HAVE_LCHOWN 1
#define HAVE_LCHMOD 1
#define HAVE_STRMODE 1
#define UID_STRING "u"
#define UID_CAST (unsigned)
#define GID_STRING "u"
#define GID_CAST (unsigned)
#define INO_STRING "llu"
#define INO_HEXSTRING "llx"
#define INO_CAST (unsigned long long)
#define DEV_STRING "u"
#define DEV_HEXSTRING "x"
#define DEV_CAST (unsigned)
CFG
CF=(-DHAVE_CONFIG_H -I"$src/xarinc" -I"$r/include" -I"$r/lib" -I"$M/libs/xar-shim" -I"$x/include" -I"$inc" -include sys/param.h -include time.h -include stdlib.h -include string.h -include unistd.h)
dylib xar "$pfx/libxar.1.dylib" 1.0.0 1.3.0 \
  "$r"/lib/{archive,arcmod,b64,bzxar,darwinattr,data,ea,err,ext2,fbsdattr,filetree,io,lzmaxar,linuxattr,hash,signature,stat,subdoc,util,zxar,script,macho}.c "$M/libs/xar-shim/evp-shim.c" \
  -- "$lib/libxml2.2.dylib" "$lib/libbz2.1.0.dylib" "${libz[@]}"
rm -rf "$inc/xar"; mkdir -p "$inc/xar"; cp "$src/xarinc/xar/xar.h" "$inc/xar/"

for n in sqlite3:libsqlite3.dylib xml2:libxml2.dylib bz2:libbz2.dylib pcre:libpcre.dylib xar:libxar.dylib; do :; done
ln -sf libxml2.2.dylib "$lib/libxml2.dylib"; ln -sf libbz2.1.0.dylib "$lib/libbz2.dylib"
ln -sf libpcre.0.dylib "$lib/libpcre.dylib"; ln -sf libxar.1.dylib "$lib/libxar.dylib"

# Prüfprogramm, gegen die eben gebauten Bibliotheken gelinkt (Installnamen /usr/lib/… bzw. $pfx)
if [ $mode = host ]; then probe="$out/libstest"; else probe="$root/Library/ClokeOS/Probe/libstest"; fi
mkdir -p "$(dirname "$probe")"
"${cc[@]}" -O1 -w -I"$inc" -I"$inc/libxml" ${ld[@]+"${ld[@]}"} -o "$probe" "$M/libs/libstest.c" \
  "$lib/libsqlite3.dylib" "$lib/libxml2.2.dylib" "$lib/libbz2.1.0.dylib" "$lib/libpcre.0.dylib" "$lib/libxar.1.dylib" ${libsys[@]+"${libsys[@]}"} 2>&1 | grep -v '^ld: warning' || true
rm -rf "$src"
echo "Fertig: $lib"
if [ $mode = host ]; then "$probe" ${2:+"$2"}; fi
